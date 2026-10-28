/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_scan_utils.h"

#include "sql/engine/basic/ob_file_parquet_reader.h"
#include "lib/file/file_directory_utils.h"
#include "lib/string/ob_string_buffer.h"
#include "lib/utility/utility.h"
#include "share/ob_errno.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{

const char *ObFileFormatUtil::CSV = "csv";
const char *ObFileFormatUtil::JSONL = "jsonl";
const char *ObFileFormatUtil::PARQUET = "parquet";
const char *ObFileFormatUtil::GLOB = "glob";

static const int64_t FILE_SCAN_BUFFER_SIZE = 64 * 1024;
static const int64_t USECS_PER_DAY_LOCAL = 24LL * 3600 * 1000 * 1000;

/* the magic prefix of an encoded file table descriptor */
static const char FILE_DESCRIPTOR_MAGIC[] = "\x01OBFILE\x01";

/* -------------------------------------------------------------------------- */
/* type utilities                                                             */
/* -------------------------------------------------------------------------- */

const char *ObFileTypeUtil::type_name(ObFileInferredType type)
{
  switch (type) {
    case OB_FILE_TYPE_NULL:    return "VARCHAR";
    case OB_FILE_TYPE_BOOLEAN: return "BOOLEAN";
    case OB_FILE_TYPE_BIGINT:  return "BIGINT";
    case OB_FILE_TYPE_DOUBLE:  return "DOUBLE";
    case OB_FILE_TYPE_DATE:    return "DATE";
    case OB_FILE_TYPE_DATETIME: return "DATETIME";
    case OB_FILE_TYPE_VARCHAR: return "VARCHAR";
    default:                   return "VARCHAR";
  }
}

ObFileInferredType ObFileTypeUtil::merge_type(ObFileInferredType lhs, ObFileInferredType rhs)
{
  if (lhs == rhs) {
    return lhs;
  } else if (OB_FILE_TYPE_NULL == lhs) {
    return rhs;
  } else if (OB_FILE_TYPE_NULL == rhs) {
    return lhs;
  } else if (OB_FILE_TYPE_VARCHAR == lhs || OB_FILE_TYPE_VARCHAR == rhs) {
    return OB_FILE_TYPE_VARCHAR;
  }
  const bool lhs_num = (OB_FILE_TYPE_BOOLEAN == lhs || OB_FILE_TYPE_BIGINT == lhs || OB_FILE_TYPE_DOUBLE == lhs);
  const bool rhs_num = (OB_FILE_TYPE_BOOLEAN == rhs || OB_FILE_TYPE_BIGINT == rhs || OB_FILE_TYPE_DOUBLE == rhs);
  if (lhs_num && rhs_num) {
    if (OB_FILE_TYPE_DOUBLE == lhs || OB_FILE_TYPE_DOUBLE == rhs) {
      return OB_FILE_TYPE_DOUBLE;
    }
    /* boolean and bigint are widened to bigint */
    return OB_FILE_TYPE_BIGINT;
  }
  if ((OB_FILE_TYPE_DATE == lhs && OB_FILE_TYPE_DATETIME == rhs)
      || (OB_FILE_TYPE_DATETIME == lhs && OB_FILE_TYPE_DATE == rhs)) {
    return OB_FILE_TYPE_DATETIME;
  }
  /* the types belong to different families, only varchar can hold both */
  return OB_FILE_TYPE_VARCHAR;
}

ObObjType ObFileTypeUtil::obj_type(ObFileInferredType type)
{
  switch (type) {
    case OB_FILE_TYPE_BOOLEAN: return ObTinyIntType;
    case OB_FILE_TYPE_BIGINT:  return ObIntType;
    case OB_FILE_TYPE_DOUBLE:  return ObDoubleType;
    case OB_FILE_TYPE_DATE:    return ObDateType;
    case OB_FILE_TYPE_DATETIME: return ObDateTimeType;
    default:                   return ObVarcharType;
  }
}

static bool is_all_digits(const char *data, int32_t len, int32_t begin)
{
  if (begin >= len) {
    return false;
  }
  for (int32_t i = begin; i < len; ++i) {
    if (data[i] < '0' || data[i] > '9') {
      return false;
    }
  }
  return true;
}

/* copy a string into the allocator and terminate it with a zero byte */
static int copy_zero_terminated(common::ObIAllocator &allocator,
                                 const char *data,
                                 int32_t len,
                                 char *&result)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(len > 0 ? len + 1 : 1));
  if (NULL == buf) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (len > 0 && NULL != data) {
      ::memcpy(buf, data, len);
    }
    buf[len] = '\0';
    result = buf;
  }
  return ret;
}

static bool is_leap_year(int64_t year)
{
  return (0 == year % 4 && 0 != year % 100) || (0 == year % 400);
}

static int32_t days_in_month(int64_t year, int64_t month)
{
  static const int32_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (2 == month && is_leap_year(year)) {
    return 29;
  }
  return days[month - 1];
}

/* the number of days between 1970-01-01 and the given civil date */
static int32_t days_from_civil(int64_t year, int64_t month, int64_t day)
{
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int32_t>(era * 146097 + doe - 719468);
}

bool ObFileTypeUtil::parse_date(const char *data, int32_t len, int32_t &date_value)
{
  bool ret = false;
  if (NULL == data || len != 10 || data[4] != '-' || data[7] != '-') {
    // do nothing
  } else if (!is_all_digits(data, 4, 0) || !is_all_digits(data, 7, 5) || !is_all_digits(data, 10, 8)) {
    // do nothing
  } else {
    const int64_t year = (data[0] - '0') * 1000 + (data[1] - '0') * 100 + (data[2] - '0') * 10 + (data[3] - '0');
    const int64_t month = (data[5] - '0') * 10 + (data[6] - '0');
    const int64_t day = (data[8] - '0') * 10 + (data[9] - '0');
    if (year < 1000 || year > 9999 || month < 1 || month > 12
        || day < 1 || day > days_in_month(year, month)) {
      // do nothing
    } else {
      date_value = days_from_civil(year, month, day);
      ret = true;
    }
  }
  return ret;
}

bool ObFileTypeUtil::parse_datetime(const char *data, int32_t len, int64_t &datetime_value)
{
  bool ret = false;
  int32_t time_begin = 10;
  if (NULL == data || len < 10) {
    // do nothing
  } else if (len > 10 && ' ' != data[10] && 'T' != data[10]) {
    // do nothing
  } else if (len > 10) {
    time_begin = 11;
  }
  int32_t date_value = 0;
  if (!parse_date(data, 10, date_value)) {
    // do nothing
  } else {
    int64_t hour = 0, minute = 0, second = 0, fraction = 0;
    bool valid = true;
    if (time_begin < len) {
      const int32_t rest = len - time_begin;
      if (rest < 8 || ':' != data[time_begin + 2] || ':' != data[time_begin + 5]) {
        valid = false;
      } else {
        hour = (data[time_begin] - '0') * 10 + (data[time_begin + 1] - '0');
        minute = (data[time_begin + 3] - '0') * 10 + (data[time_begin + 4] - '0');
        second = (data[time_begin + 6] - '0') * 10 + (data[time_begin + 7] - '0');
        int32_t pos = time_begin + 8;
        if (pos < len) {
          if ('.' != data[pos]) {
            valid = false;
          } else {
            ++pos;
            int64_t scale = 100000;
            while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
              if (scale > 0) {
                fraction += (data[pos] - '0') * scale;
                scale /= 10;
              }
              ++pos;
            }
            if (pos != len) {
              valid = false;
            }
          }
        }
        if (hour > 23 || minute > 59 || second > 59) {
          valid = false;
        }
      }
    }
    if (valid) {
      datetime_value = static_cast<int64_t>(date_value) * USECS_PER_DAY_LOCAL
                       + hour * 3600 * 1000 * 1000 + minute * 60 * 1000 * 1000
                       + second * 1000 * 1000 + fraction;
      ret = true;
    }
  }
  return ret;
}

ObFileInferredType ObFileTypeUtil::infer_scalar_type(const char *data, int32_t len)
{
  ObFileInferredType type = OB_FILE_TYPE_VARCHAR;
  if (NULL == data || len <= 0) {
    type = OB_FILE_TYPE_NULL;
  } else if (4 == len && 0 == ::strncasecmp(data, "true", 4)) {
    type = OB_FILE_TYPE_BOOLEAN;
  } else if (5 == len && 0 == ::strncasecmp(data, "false", 5)) {
    type = OB_FILE_TYPE_BOOLEAN;
  } else {
    /* try an integer */
    int32_t begin = (data[0] == '-' || data[0] == '+') ? 1 : 0;
    bool is_int = is_all_digits(data, len, begin);
    if (is_int) {
      errno = 0;
      char *endptr = NULL;
      const long long value = ::strtoll(data, &endptr, 10);
      if (0 == errno && NULL != endptr && endptr == data + len) {
        type = OB_FILE_TYPE_BIGINT;
      } else {
        /* the value does not fit into int64, fall back to double */
        errno = 0;
        endptr = NULL;
        const double dvalue = ::strtod(data, &endptr);
        if (0 == errno && NULL != endptr && endptr == data + len) {
          type = OB_FILE_TYPE_DOUBLE;
        }
      }
    } else {
      /* try a floating point number */
      errno = 0;
      char *endptr = NULL;
      const double dvalue = ::strtod(data, &endptr);
      const bool is_double = (0 == errno && NULL != endptr && endptr == data + len);
      if (!is_double) {
        int32_t date_value = 0;
        int64_t datetime_value = 0;
        if (parse_date(data, len, date_value)) {
          type = OB_FILE_TYPE_DATE;
        } else if (parse_datetime(data, len, datetime_value)) {
          type = OB_FILE_TYPE_DATETIME;
        } else {
          type = OB_FILE_TYPE_VARCHAR;
        }
      } else {
        type = OB_FILE_TYPE_DOUBLE;
      }
    }
  }
  return type;
}

int ObFileTypeUtil::convert_value(const char *data,
                                  int32_t len,
                                  ObFileInferredType type,
                                  bool is_null_text,
                                  ObFileValue &value)
{
  int ret = OB_SUCCESS;
  value.is_null_ = true;
  value.type_ = type;
  value.int_val_ = 0;
  value.double_val_ = 0.0;
  value.str_val_ = common::ObString();
  if (is_null_text) {
    // the value is null
  } else if (OB_FILE_TYPE_VARCHAR == type) {
    value.is_null_ = false;
    value.str_val_.assign_ptr(data, len);
  } else if (NULL == data || len <= 0) {
    // an empty value of a typed column is null
  } else if (OB_FILE_TYPE_BOOLEAN == type) {
    if ((4 == len && 0 == ::strncasecmp(data, "true", 4)) || (1 == len && '1' == data[0])) {
      value.is_null_ = false;
      value.int_val_ = 1;
    } else if ((5 == len && 0 == ::strncasecmp(data, "false", 5)) || (1 == len && '0' == data[0])) {
      value.is_null_ = false;
      value.int_val_ = 0;
    }
  } else if (OB_FILE_TYPE_BIGINT == type) {
    errno = 0;
    char *endptr = NULL;
    const long long lvalue = ::strtoll(data, &endptr, 10);
    if (0 == errno && NULL != endptr && endptr == data + len) {
      value.is_null_ = false;
      value.int_val_ = lvalue;
    }
  } else if (OB_FILE_TYPE_DOUBLE == type) {
    errno = 0;
    char *endptr = NULL;
    const double dvalue = ::strtod(data, &endptr);
    if (0 == errno && NULL != endptr && endptr == data + len) {
      value.is_null_ = false;
      value.double_val_ = dvalue;
    }
  } else if (OB_FILE_TYPE_DATE == type) {
    int32_t date_value = 0;
    if (parse_date(data, len, date_value)) {
      value.is_null_ = false;
      value.int_val_ = date_value;
    }
  } else if (OB_FILE_TYPE_DATETIME == type) {
    int64_t datetime_value = 0;
    if (parse_datetime(data, len, datetime_value)) {
      value.is_null_ = false;
      value.int_val_ = datetime_value;
    }
  }
  return ret;
}

/* -------------------------------------------------------------------------- */
/* descriptor encoding                                                        */
/* -------------------------------------------------------------------------- */

static int append_kv(common::ObStringBuffer &buf,
                     const char *key,
                     const char *value,
                     int32_t value_len)
{
  int ret = OB_SUCCESS;
  char len_buf[32];
  const int32_t len_len = static_cast<int32_t>(snprintf(len_buf, sizeof(len_buf), "%d", value_len));
  if (OB_FAIL(buf.append(key))) {
  } else if (OB_FAIL(buf.append("="))) {
  } else if (OB_FAIL(buf.append(len_buf, len_len))) {
  } else if (OB_FAIL(buf.append(":"))) {
  } else if (OB_FAIL(buf.append(value, value_len))) {
  } else if (OB_FAIL(buf.append(";"))) {
  }
  return ret;
}

static int append_kv(common::ObStringBuffer &buf, const char *key, int64_t value)
{
  int ret = OB_SUCCESS;
  char tmp[32];
  const int32_t len = static_cast<int32_t>(snprintf(tmp, sizeof(tmp), "%ld", value));
  if (OB_FAIL(append_kv(buf, key, tmp, len))) {
  }
  return ret;
}

int ObFileTableDescriptor::encode(common::ObIAllocator &allocator, common::ObString &result) const
{
  int ret = OB_SUCCESS;
  common::ObStringBuffer buf(&allocator);
  const char *format_name = ObFileFormatUtil::CSV;
  switch (format_) {
    case OB_FILE_FORMAT_JSONL:   format_name = ObFileFormatUtil::JSONL; break;
    case OB_FILE_FORMAT_PARQUET: format_name = ObFileFormatUtil::PARQUET; break;
    case OB_FILE_FORMAT_GLOB:    format_name = ObFileFormatUtil::GLOB; break;
    default:                     format_name = ObFileFormatUtil::CSV; break;
  }
  if (OB_FAIL(buf.append(FILE_DESCRIPTOR_MAGIC, sizeof(FILE_DESCRIPTOR_MAGIC) - 1))) {
  } else if (OB_FAIL(append_kv(buf, "fmt", format_name, static_cast<int32_t>(strlen(format_name))))) {
  } else if (OB_FAIL(append_kv(buf, "path", path_.ptr(), path_.length()))) {
  } else if (OB_FAIL(append_kv(buf, "hdr", options_.has_header_ ? 1 : 0))) {
  } else if (OB_FAIL(append_kv(buf, "del", &options_.delimiter_, 1))) {
  } else if (OB_FAIL(append_kv(buf, "quo", &options_.quote_char_, 1))) {
  } else if (OB_FAIL(append_kv(buf, "esc", &options_.escape_char_, 1))) {
  } else if (OB_FAIL(append_kv(buf, "smp", options_.sample_size_))) {
  } else if (OB_FAIL(append_kv(buf, "nul", options_.nullstr_.ptr(), options_.nullstr_.length()))) {
  } else if (OB_FAIL(append_kv(buf, "fop", options_.format_option_.ptr(), options_.format_option_.length()))) {
  } else if (OB_FAIL(append_kv(buf, "fsz", file_size_))) {
  } else if (OB_FAIL(append_kv(buf, "fmt2", file_mtime_))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
      const ObFileColumnSchema &col = columns_.at(i);
      common::ObStringBuffer colbuf(&allocator);
      if (OB_FAIL(append_kv(colbuf, "nm", col.name_.ptr(), col.name_.length()))) {
      } else if (OB_FAIL(append_kv(colbuf, "tp", static_cast<int64_t>(col.type_)))) {
      } else if (OB_FAIL(append_kv(colbuf, "nu", col.nullable_ ? 1 : 0))) {
      } else if (OB_FAIL(append_kv(buf, "col", colbuf.ptr(), colbuf.length()))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    result.assign_ptr(buf.ptr(), static_cast<int32_t>(buf.length()));
  }
  return ret;
}

namespace
{
class ObFileDescriptorDecoder
{
public:
  ObFileDescriptorDecoder(const char *data, int32_t len) : data_(data), len_(len), pos_(0) {}
  bool next(const char *&key, int32_t &key_len, const char *&value, int32_t &value_len)
  {
    if (pos_ >= len_) {
      return false;
    }
    const char *eq = static_cast<const char *>(::memchr(data_ + pos_, '=', len_ - pos_));
    if (NULL == eq) {
      return false;
    }
    key = data_ + pos_;
    key_len = static_cast<int32_t>(eq - key);
    int32_t p = pos_ + key_len + 1;
    int64_t vlen = 0;
    int32_t digits = 0;
    while (p < len_ && data_[p] >= '0' && data_[p] <= '9') {
      vlen = vlen * 10 + (data_[p] - '0');
      ++p;
      ++digits;
    }
    if (0 == digits || p >= len_ || ':' != data_[p]) {
      return false;
    }
    ++p;
    if (p + vlen > len_) {
      return false;
    }
    value = data_ + p;
    value_len = static_cast<int32_t>(vlen);
    pos_ = p + vlen;
    if (pos_ < len_ && ';' == data_[pos_]) {
      ++pos_;
    }
    return true;
  }

private:
  const char *data_;
  int32_t len_;
  int32_t pos_;
};
} // namespace

int ObFileTableDescriptor::decode(common::ObIAllocator &allocator,
                                  const common::ObString &str,
                                  ObFileTableDescriptor &desc)
{
  int ret = OB_SUCCESS;
  const int32_t magic_len = sizeof(FILE_DESCRIPTOR_MAGIC) - 1;
  if (str.length() < magic_len || 0 != ::memcmp(str.ptr(), FILE_DESCRIPTOR_MAGIC, magic_len)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid file table descriptor", K(ret), K(str.length()));
  } else {
    ObFileDescriptorDecoder decoder(str.ptr() + magic_len, str.length() - magic_len);
    const char *key = NULL;
    const char *value = NULL;
    int32_t key_len = 0;
    int32_t value_len = 0;
    ObSEArray<ObFileColumnSchema, 16> columns;
    while (OB_SUCC(ret) && decoder.next(key, key_len, value, value_len)) {
      const common::ObString k(key_len, key);
      if (k.compare_equal("fmt")) {
        const common::ObString v(value_len, value);
        if (v.compare_equal(ObFileFormatUtil::JSONL)) {
          desc.format_ = OB_FILE_FORMAT_JSONL;
        } else if (v.compare_equal(ObFileFormatUtil::PARQUET)) {
          desc.format_ = OB_FILE_FORMAT_PARQUET;
        } else if (v.compare_equal(ObFileFormatUtil::GLOB)) {
          desc.format_ = OB_FILE_FORMAT_GLOB;
        } else {
          desc.format_ = OB_FILE_FORMAT_CSV;
        }
      } else if (k.compare_equal("path")) {
        char *buf = static_cast<char *>(allocator.alloc(value_len > 0 ? value_len : 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          if (value_len > 0) {
            ::memcpy(buf, value, value_len);
          }
          desc.path_.assign_ptr(buf, value_len);
        }
      } else if (k.compare_equal("hdr")) {
        desc.options_.has_header_ = (value_len > 0 && '1' == value[0]);
      } else if (k.compare_equal("del")) {
        desc.options_.delimiter_ = (value_len > 0) ? value[0] : ',';
      } else if (k.compare_equal("quo")) {
        desc.options_.quote_char_ = (value_len > 0) ? value[0] : '"';
      } else if (k.compare_equal("esc")) {
        desc.options_.escape_char_ = (value_len > 0) ? value[0] : '"';
      } else if (k.compare_equal("smp")) {
        desc.options_.sample_size_ = ::atoll(value);
      } else if (k.compare_equal("nul")) {
        char *buf = static_cast<char *>(allocator.alloc(value_len > 0 ? value_len : 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          if (value_len > 0) {
            ::memcpy(buf, value, value_len);
          }
          desc.options_.nullstr_.assign_ptr(buf, value_len);
        }
      } else if (k.compare_equal("fop")) {
        char *buf = static_cast<char *>(allocator.alloc(value_len > 0 ? value_len : 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          if (value_len > 0) {
            ::memcpy(buf, value, value_len);
          }
          desc.options_.format_option_.assign_ptr(buf, value_len);
        }
      } else if (k.compare_equal("fsz")) {
        desc.file_size_ = ::atoll(value);
      } else if (k.compare_equal("fmt2")) {
        desc.file_mtime_ = ::atoll(value);
      } else if (k.compare_equal("col")) {
        ObFileColumnSchema col;
        ObFileDescriptorDecoder col_decoder(value, value_len);
        const char *ckey = NULL;
        const char *cvalue = NULL;
        int32_t ckey_len = 0;
        int32_t cvalue_len = 0;
        while (OB_SUCC(ret) && col_decoder.next(ckey, ckey_len, cvalue, cvalue_len)) {
          const common::ObString ck(ckey_len, ckey);
          if (ck.compare_equal("nm")) {
            char *buf = static_cast<char *>(allocator.alloc(cvalue_len > 0 ? cvalue_len : 1));
            if (NULL == buf) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              if (cvalue_len > 0) {
                ::memcpy(buf, cvalue, cvalue_len);
              }
              col.name_.assign_ptr(buf, cvalue_len);
            }
          } else if (ck.compare_equal("tp")) {
            col.type_ = static_cast<ObFileInferredType>(::atoi(cvalue));
          } else if (ck.compare_equal("nu")) {
            col.nullable_ = (cvalue_len > 0 && '1' == cvalue[0]);
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(columns.push_back(col))) {
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(desc.columns_.assign(columns))) {
    }
  }
  return ret;
}

bool ObFileTableDescriptor::is_file_descriptor(const common::ObString &str)
{
  const int32_t magic_len = sizeof(FILE_DESCRIPTOR_MAGIC) - 1;
  return str.length() >= magic_len && 0 == ::memcmp(str.ptr(), FILE_DESCRIPTOR_MAGIC, magic_len);
}

/* -------------------------------------------------------------------------- */
/* file helpers                                                               */
/* -------------------------------------------------------------------------- */

int ObFileScanUtils::check_file_format(const common::ObString &path, ObFileTableFormat &format)
{
  int ret = OB_SUCCESS;
  const char *suffix = NULL;
  if (path.length() > 4 && 0 == ::strncasecmp(path.ptr() + path.length() - 4, ".csv", 4)) {
    suffix = ".csv";
  } else if (path.length() > 5 && 0 == ::strncasecmp(path.ptr() + path.length() - 5, ".jsonl", 5)) {
    suffix = ".jsonl";
  } else if (path.length() > 5 && 0 == ::strncasecmp(path.ptr() + path.length() - 5, ".ndjson", 5)) {
    suffix = ".ndjson";
  } else if (path.length() > 5 && 0 == ::strncasecmp(path.ptr() + path.length() - 5, ".json", 5)) {
    suffix = ".json";
  } else if (path.length() > 8 && 0 == ::strncasecmp(path.ptr() + path.length() - 8, ".parquet", 8)) {
    suffix = ".parquet";
  }
  if (NULL == suffix) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "unsupported file extension of file table");
  } else {
    if (0 == ::strcmp(suffix, ".csv")) {
      format = OB_FILE_FORMAT_CSV;
    } else if (0 == ::strcmp(suffix, ".parquet")) {
      format = OB_FILE_FORMAT_PARQUET;
    } else {
      format = OB_FILE_FORMAT_JSONL;
    }
  }
  return ret;
}

int ObFileScanUtils::stat_file(const common::ObString &path, int64_t &file_size, int64_t &file_mtime)
{
  int ret = OB_SUCCESS;
  struct stat stat_buf;
  if (NULL == path.ptr() || path.length() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    /* the path is not null terminated, it may come from the encoded file descriptor */
    char *path_buf = static_cast<char *>(::malloc(path.length() + 1));
    if (NULL == path_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ::memcpy(path_buf, path.ptr(), path.length());
      path_buf[path.length()] = '\0';
      if (0 != ::stat(path_buf, &stat_buf)) {
        ret = OB_FILE_NOT_EXIST;
        LOG_USER_ERROR(OB_FILE_NOT_EXIST, "the file of the file table does not exist");
      }
      ::free(path_buf);
    }
  }
  if (OB_SUCC(ret) && !S_ISREG(stat_buf.st_mode)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the path of the file table is not a regular file");
  } else if (OB_SUCC(ret) && stat_buf.st_size > MAX_FILE_SIZE) {
    ret = OB_SIZE_OVERFLOW;
    LOG_USER_ERROR(OB_SIZE_OVERFLOW, "the file of the file table is too large");
  } else if (OB_SUCC(ret)) {
    file_size = stat_buf.st_size;
    file_mtime = static_cast<int64_t>(stat_buf.st_mtime);
  }
  return ret;
}

int ObFileScanUtils::expand_glob(common::ObIAllocator &allocator,
                                 const common::ObString &pattern,
                                 common::ObSEArray<common::ObString, 16> &files)
{
  int ret = OB_SUCCESS;
  common::ObString dir_part;
  common::ObString file_part;
  const char *slash = NULL;
  for (int32_t i = pattern.length() - 1; i >= 0; --i) {
    if ('/' == pattern.ptr()[i]) {
      slash = pattern.ptr() + i;
      break;
    }
  }
  if (NULL == slash) {
    dir_part = common::ObString(".");
    file_part = pattern;
  } else {
    dir_part.assign_ptr(pattern.ptr(), static_cast<int32_t>(slash - pattern.ptr()));
    if (0 == dir_part.length()) {
      dir_part = common::ObString("/");
    }
    file_part.assign_ptr(slash + 1, pattern.length() - static_cast<int32_t>(slash + 1 - pattern.ptr()));
  }
  /* opendir and fnmatch need zero terminated strings */
  char *dir_path = NULL;
  char *file_pattern = NULL;
  if (OB_FAIL(copy_zero_terminated(allocator, dir_part.ptr(), dir_part.length(), dir_path))) {
  } else if (OB_FAIL(copy_zero_terminated(allocator, file_part.ptr(), file_part.length(), file_pattern))) {
  } else {
    DIR *dir = ::opendir(dir_path);
    if (NULL == dir) {
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST, "the directory of the glob pattern does not exist");
    } else {
      struct dirent *entry = NULL;
      while (NULL != (entry = ::readdir(dir))) {
        if (0 == ::strcmp(entry->d_name, ".") || 0 == ::strcmp(entry->d_name, "..")) {
          continue;
        }
        if (0 != ::fnmatch(file_pattern, entry->d_name, 0)) {
          continue;
        }
        const int64_t name_len = static_cast<int64_t>(::strlen(entry->d_name));
        const int64_t dir_len = dir_part.length();
        char *full = static_cast<char *>(allocator.alloc(dir_len + 1 + name_len + 1));
        if (NULL == full) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          break;
        }
        ::memcpy(full, dir_part.ptr(), dir_len);
        full[dir_len] = '/';
        ::memcpy(full + dir_len + 1, entry->d_name, name_len);
        full[dir_len + 1 + name_len] = '\0';
        if (OB_FAIL(files.push_back(common::ObString(static_cast<int32_t>(dir_len + 1 + name_len), full)))) {
          break;
        }
        if (files.count() > MAX_GLOB_FILES) {
          ret = OB_SIZE_OVERFLOW;
          LOG_USER_ERROR(OB_SIZE_OVERFLOW, "too many files matched by the glob pattern");
          break;
        }
      }
      ::closedir(dir);
    }
  }
  return ret;
}

/* the names of the columns are not terminated, so they are compared by the length */
static bool column_name_equal(const common::ObString &left, const common::ObString &right)
{
  if (left.length() != right.length()) {
    return false;
  }
  for (int32_t i = 0; i < left.length(); ++i) {
    char l = left.ptr()[i];
    char r = right.ptr()[i];
    if (l >= 'A' && l <= 'Z') {
      l = static_cast<char>(l - 'A' + 'a');
    }
    if (r >= 'A' && r <= 'Z') {
      r = static_cast<char>(r - 'A' + 'a');
    }
    if (l != r) {
      return false;
    }
  }
  return true;
}

void ObFileScanUtils::uniquify_column_names(common::ObIAllocator &allocator,
                                            common::ObSEArray<ObFileColumnSchema, 16> &columns)
{
  for (int64_t i = 0; i < columns.count(); ++i) {
    ObFileColumnSchema &col = columns.at(i);
    bool duplicated = false;
    for (int64_t j = 0; j < i; ++j) {
      if (column_name_equal(columns.at(j).name_, col.name_)) {
        duplicated = true;
        break;
      }
    }
    if (duplicated) {
      for (int32_t suffix = 2; suffix < 1000; ++suffix) {
        char tmp[1024];
        const int32_t len = snprintf(tmp, sizeof(tmp), "%.*s__%d",
                                     col.name_.length(), col.name_.ptr(), suffix);
        if (len <= 0 || len >= static_cast<int32_t>(sizeof(tmp))) {
          break;
        }
        bool exist = false;
        for (int64_t j = 0; j < i; ++j) {
          if (column_name_equal(columns.at(j).name_, common::ObString(len, tmp))) {
            exist = true;
            break;
          }
        }
        if (!exist) {
          char *buf = static_cast<char *>(allocator.alloc(len));
          if (NULL != buf) {
            ::memcpy(buf, tmp, len);
            col.name_.assign_ptr(buf, len);
          }
          break;
        }
      }
    }
  }
}

/* -------------------------------------------------------------------------- */
/* the buffered file reader                                                   */
/* -------------------------------------------------------------------------- */

ObFileLineReader::ObFileLineReader()
    : fd_(-1),
      file_size_(0),
      buf_(NULL),
      buf_cap_(0),
      buf_len_(0),
      buf_pos_(0),
      eof_(false),
      record_no_(0),
      quote_char_(0),
      escape_char_(0),
      in_quote_(false),
      pending_quote_(false),
      pending_escape_(false),
      skip_lf_(false),
      rec_buf_(NULL),
      rec_cap_(0),
      rec_len_(0)
{}

ObFileLineReader::~ObFileLineReader()
{
  close();
}

void ObFileLineReader::set_csv_params(char quote_char, char escape_char)
{
  quote_char_ = quote_char;
  escape_char_ = escape_char;
}

int ObFileLineReader::open(const common::ObString &path)
{
  int ret = OB_SUCCESS;
  close();
  if (NULL == path.ptr() || path.length() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char *path_buf = static_cast<char *>(::malloc(path.length() + 1));
    if (NULL == path_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ::memcpy(path_buf, path.ptr(), path.length());
      path_buf[path.length()] = '\0';
      ret = FileDirectoryUtils::open(path_buf, O_RDONLY, 0, fd_);
      if (OB_SUCC(ret)) {
        file_size_ = 0;
        int64_t size = 0;
        if (OB_SUCC(FileDirectoryUtils::get_file_size(path_buf, size))) {
          file_size_ = size;
        }
        buf_cap_ = FILE_SCAN_BUFFER_SIZE;
        buf_ = static_cast<char *>(::malloc(buf_cap_));
        if (NULL == buf_) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          ::close(fd_);
          fd_ = -1;
        } else {
          buf_len_ = 0;
          buf_pos_ = 0;
          eof_ = false;
          record_no_ = 0;
          in_quote_ = false;
          pending_quote_ = false;
          pending_escape_ = false;
          skip_lf_ = false;
          rec_len_ = 0;
        }
      }
      ::free(path_buf);
    }
  }
  return ret;
}

void ObFileLineReader::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (NULL != buf_) {
    ::free(buf_);
    buf_ = NULL;
  }
  if (NULL != rec_buf_) {
    ::free(rec_buf_);
    rec_buf_ = NULL;
  }
  buf_cap_ = 0;
  buf_len_ = 0;
  buf_pos_ = 0;
  rec_cap_ = 0;
  rec_len_ = 0;
}

int ObFileLineReader::read_more()
{
  int ret = OB_SUCCESS;
  if (eof_) {
    // do nothing
  } else {
    const ssize_t n = ::read(fd_, buf_ + buf_len_, buf_cap_ - buf_len_);
    if (n < 0) {
      if (EINTR == errno) {
        ret = OB_SUCCESS;
      } else {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to read the file of the file table", K(ret), K(errno));
      }
    } else if (0 == n) {
      eof_ = true;
    } else {
      buf_len_ += n;
    }
  }
  return ret;
}

int ObFileLineReader::compact_buffer()
{
  int ret = OB_SUCCESS;
  if (buf_pos_ > 0) {
    const int64_t remain = buf_len_ - buf_pos_;
    if (remain > 0) {
      ::memmove(buf_, buf_ + buf_pos_, remain);
    }
    buf_len_ = remain;
    buf_pos_ = 0;
  }
  return ret;
}

int ObFileLineReader::ensure_buffer(int64_t extra)
{
  int ret = OB_SUCCESS;
  if (buf_len_ + extra > buf_cap_) {
    int64_t new_cap = buf_cap_ * 2;
    while (new_cap < buf_len_ + extra) {
      new_cap *= 2;
    }
    char *new_buf = static_cast<char *>(::realloc(buf_, new_cap));
    if (NULL == new_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      buf_ = new_buf;
      buf_cap_ = new_cap;
    }
  }
  return ret;
}

int ObFileLineReader::append_char(char c)
{
  int ret = OB_SUCCESS;
  if (rec_len_ + 1 > rec_cap_) {
    const int64_t new_cap = (rec_cap_ == 0) ? 256 : rec_cap_ * 2;
    char *new_buf = static_cast<char *>(::realloc(rec_buf_, new_cap));
    if (NULL == new_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      rec_buf_ = new_buf;
      rec_cap_ = new_cap;
    }
  }
  if (OB_SUCC(ret)) {
    rec_buf_[rec_len_++] = c;
  }
  return ret;
}

int ObFileLineReader::append_bytes(const char *data, int64_t len)
{
  int ret = OB_SUCCESS;
  if (rec_len_ + len > rec_cap_) {
    int64_t new_cap = (rec_cap_ == 0) ? 256 : rec_cap_;
    while (new_cap < rec_len_ + len) {
      new_cap *= 2;
    }
    char *new_buf = static_cast<char *>(::realloc(rec_buf_, new_cap));
    if (NULL == new_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      rec_buf_ = new_buf;
      rec_cap_ = new_cap;
    }
  }
  if (OB_SUCC(ret) && len > 0) {
    ::memcpy(rec_buf_ + rec_len_, data, len);
    rec_len_ += len;
  }
  return ret;
}

int ObFileLineReader::next_record(common::ObString &record, int64_t &record_no)
{
  int ret = OB_SUCCESS;
  bool done = false;
  rec_len_ = 0;
  in_quote_ = false;
  pending_quote_ = false;
  pending_escape_ = false;
  while (OB_SUCC(ret) && !done) {
    if (buf_pos_ >= buf_len_) {
      if (eof_) {
        if (rec_len_ > 0) {
          done = true;
        } else {
          ret = OB_ITER_END;
        }
        break;
      }
      if (OB_FAIL(compact_buffer())) {
        break;
      }
      if (OB_FAIL(ensure_buffer(FILE_SCAN_BUFFER_SIZE))) {
        break;
      }
      if (OB_FAIL(read_more())) {
        break;
      }
      if (0 == buf_len_ && eof_) {
        if (rec_len_ > 0) {
          done = true;
        } else {
          ret = OB_ITER_END;
        }
        break;
      }
      continue;
    }
    if (skip_lf_) {
      skip_lf_ = false;
      if ('\n' == buf_[buf_pos_]) {
        ++buf_pos_;
        continue;
      }
    }
    while (buf_pos_ < buf_len_) {
      const char c = buf_[buf_pos_];
      if (pending_escape_) {
        pending_escape_ = false;
        if (OB_FAIL(append_char(c))) {
          break;
        }
        ++buf_pos_;
        continue;
      }
      if (pending_quote_) {
        pending_quote_ = false;
        if (c == quote_char_) {
          /* a doubled quote inside a quoted section */
          if (OB_FAIL(append_char(c))) {
            break;
          }
          ++buf_pos_;
          continue;
        }
        /* the quoted section is closed */
        in_quote_ = false;
      }
      if (in_quote_) {
        if (0 != escape_char_ && escape_char_ != quote_char_ && c == escape_char_) {
          if (OB_FAIL(append_char(c))) {
            break;
          }
          ++buf_pos_;
          if (buf_pos_ < buf_len_) {
            if (OB_FAIL(append_char(buf_[buf_pos_]))) {
              break;
            }
            ++buf_pos_;
          } else {
            pending_escape_ = true;
          }
          continue;
        }
        if (c == quote_char_) {
          if (OB_FAIL(append_char(c))) {
            break;
          }
          ++buf_pos_;
          if (buf_pos_ < buf_len_) {
            if (buf_[buf_pos_] == quote_char_) {
              if (OB_FAIL(append_char(c))) {
                break;
              }
              ++buf_pos_;
              continue;
            }
            /* the quoted section is closed */
            in_quote_ = false;
            continue;
          }
          /* the next byte decides whether the quote is doubled or closes the section */
          pending_quote_ = true;
          continue;
        }
        if (OB_FAIL(append_char(c))) {
          break;
        }
        ++buf_pos_;
        continue;
      }
      if (0 != quote_char_ && c == quote_char_) {
        in_quote_ = true;
        if (OB_FAIL(append_char(c))) {
          break;
        }
        ++buf_pos_;
        continue;
      }
      if ('\n' == c) {
        ++buf_pos_;
        done = true;
        break;
      }
      if ('\r' == c) {
        ++buf_pos_;
        skip_lf_ = true;
        done = true;
        break;
      }
      if (OB_FAIL(append_char(c))) {
        break;
      }
      ++buf_pos_;
    }
  }
  if (OB_SUCC(ret)) {
    /* the record is terminated so that the parsers of the values do not read
       the bytes which follow the record */
    if (OB_FAIL(append_char('\0'))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      --rec_len_;
      record.assign_ptr(rec_buf_, static_cast<int32_t>(rec_len_));
      record_no = ++record_no_;
    }
  }
  return ret;
}

/* -------------------------------------------------------------------------- */
/* the csv splitter                                                           */
/* -------------------------------------------------------------------------- */

ObFileCsvSplitter::ObFileCsvSplitter(char delimiter, char quote_char, char escape_char)
    : delimiter_(delimiter),
      quote_char_(quote_char),
      escape_char_(escape_char),
      buf_(NULL),
      buf_cap_(0),
      buf_len_(0)
{}

ObFileCsvSplitter::~ObFileCsvSplitter()
{
  if (NULL != buf_) {
    ::free(buf_);
    buf_ = NULL;
  }
}

int ObFileCsvSplitter::append(const char *data, int32_t len)
{
  int ret = OB_SUCCESS;
  if (buf_len_ + len > buf_cap_) {
    int64_t new_cap = (buf_cap_ == 0) ? 256 : buf_cap_;
    while (new_cap < buf_len_ + len) {
      new_cap *= 2;
    }
    char *new_buf = static_cast<char *>(::realloc(buf_, new_cap));
    if (NULL == new_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      buf_ = new_buf;
      buf_cap_ = new_cap;
    }
  }
  if (OB_SUCC(ret) && len > 0) {
    ::memcpy(buf_ + buf_len_, data, len);
    buf_len_ += len;
  }
  return ret;
}

int ObFileCsvSplitter::ensure_capacity(int64_t capacity)
{
  int ret = OB_SUCCESS;
  if (capacity > buf_cap_) {
    char *new_buf = static_cast<char *>(::realloc(buf_, static_cast<size_t>(capacity)));
    if (NULL == new_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      buf_ = new_buf;
      buf_cap_ = capacity;
    }
  }
  return ret;
}

int ObFileCsvSplitter::split(const common::ObString &record, common::ObSEArray<ObFileCsvField, 16> &fields)
{
  int ret = OB_SUCCESS;
  buf_len_ = 0;
  fields.reset();
  int32_t pos = 0;
  const int32_t len = record.length();
  const char *data = record.ptr();
  /* the fields hold at most the characters of the record and every field is
     terminated by a zero byte, the buffer never moves while the record is
     split so the pointers of the fields stay valid */
  if (OB_FAIL(ensure_capacity(2 * len + 2))) {
    return ret;
  }
  while (OB_SUCC(ret)) {
    const int64_t field_begin = buf_len_;
    bool is_quoted = false;
    if (pos < len && 0 != quote_char_ && quote_char_ == data[pos]) {
      is_quoted = true;
      ++pos;
      while (pos < len) {
        const char c = data[pos];
        if (0 != escape_char_ && escape_char_ != quote_char_ && c == escape_char_) {
          if (OB_FAIL(append(&c, 1))) {
            break;
          }
          ++pos;
          if (pos < len) {
            if (OB_FAIL(append(data + pos, 1))) {
              break;
            }
            ++pos;
          }
          continue;
        }
        if (0 != quote_char_ && c == quote_char_) {
          if (pos + 1 < len && quote_char_ == data[pos + 1]) {
            if (OB_FAIL(append(&c, 1))) {
              break;
            }
            ++pos;
            ++pos;
            continue;
          }
          ++pos;
          break;
        }
        if (OB_FAIL(append(&c, 1))) {
          break;
        }
        ++pos;
      }
      /* the characters which follow the closing quote belong to the field */
      while (pos < len && delimiter_ != data[pos]) {
        if (OB_FAIL(append(data + pos, 1))) {
          break;
        }
        ++pos;
      }
    } else {
      while (pos < len && delimiter_ != data[pos]) {
        if (OB_FAIL(append(data + pos, 1))) {
          break;
        }
        ++pos;
      }
    }
    if (OB_SUCC(ret)) {
      ObFileCsvField field;
      field.is_quoted_ = is_quoted;
      field.value_.assign_ptr(buf_ + field_begin, static_cast<int32_t>(buf_len_ - field_begin));
      if (OB_FAIL(fields.push_back(field))) {
        break;
      }
      /* the numeric values are parsed with the c library, the fields have to be
         terminated so that the bytes which follow them are not read */
      char zero = '\0';
      if (OB_FAIL(append(&zero, 1))) {
        break;
      }
    }
    if (OB_SUCC(ret) && pos < len && delimiter_ == data[pos]) {
      ++pos;
      continue;
    }
    break;
  }
  return ret;
}

/* -------------------------------------------------------------------------- */
/* the flat json parser                                                       */
/* -------------------------------------------------------------------------- */

ObFileJsonParser::~ObFileJsonParser() {}

static void skip_spaces(const char *data, int32_t len, int32_t &pos)
{
  while (pos < len && (' ' == data[pos] || '\t' == data[pos] || '\r' == data[pos] || '\n' == data[pos])) {
    ++pos;
  }
}

int ObFileJsonParser::parse_string(const char *data,
                                   int32_t len,
                                   int32_t &pos,
                                   common::ObString &result,
                                   common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (pos >= len || '"' != data[pos]) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "invalid json string of the file table");
  } else {
    ++pos;
    const int32_t begin = pos;
    bool has_escape = false;
    while (pos < len && '"' != data[pos]) {
      if ('\\' == data[pos]) {
        has_escape = true;
        ++pos;
        if (pos < len) {
          ++pos;
        }
      } else {
        ++pos;
      }
    }
    if (pos >= len) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "invalid json string of the file table");
    } else {
      const int32_t raw_len = pos - begin;
      ++pos;
      if (!has_escape) {
        result.assign_ptr(data + begin, raw_len);
      } else {
        char *buf = static_cast<char *>(allocator.alloc(raw_len > 0 ? raw_len : 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          int32_t out = 0;
          for (int32_t i = 0; i < raw_len; ++i) {
            if ('\\' == data[begin + i] && i + 1 < raw_len) {
              ++i;
              switch (data[begin + i]) {
                case 'n': buf[out++] = '\n'; break;
                case 't': buf[out++] = '\t'; break;
                case 'r': buf[out++] = '\r'; break;
                case 'b': buf[out++] = '\b'; break;
                case 'f': buf[out++] = '\f'; break;
                case '/': buf[out++] = '/'; break;
                case '"': buf[out++] = '"'; break;
                case '\\': buf[out++] = '\\'; break;
                default: buf[out++] = data[begin + i]; break;
              }
            } else {
              buf[out++] = data[begin + i];
            }
          }
          result.assign_ptr(buf, out);
        }
      }
    }
  }
  return ret;
}

int ObFileJsonParser::parse_number(const char *data, int32_t len, int32_t &pos, ObFileJsonMember &member)
{
  int ret = OB_SUCCESS;
  const int32_t begin = pos;
  bool is_int = true;
  if (pos < len && ('-' == data[pos] || '+' == data[pos])) {
    ++pos;
  }
  while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
    ++pos;
  }
  if (pos < len && '.' == data[pos]) {
    is_int = false;
    ++pos;
    while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
      ++pos;
    }
  }
  if (pos < len && ('e' == data[pos] || 'E' == data[pos])) {
    is_int = false;
    ++pos;
    if (pos < len && ('-' == data[pos] || '+' == data[pos])) {
      ++pos;
    }
    while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
      ++pos;
    }
  }
  if (pos == begin) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "invalid json number of the file table");
  } else {
    member.is_null_ = false;
    member.is_quoted_ = false;
    member.is_int_ = is_int;
    member.text_.assign_ptr(data + begin, pos - begin);
    if (is_int) {
      errno = 0;
      char *endptr = NULL;
      const long long lvalue = ::strtoll(member.text_.ptr(), &endptr, 10);
      if (0 == errno && NULL != endptr && endptr == member.text_.ptr() + member.text_.length()) {
        member.int_val_ = lvalue;
      } else {
        member.is_int_ = false;
      }
    }
    if (!member.is_int_) {
      errno = 0;
      char *endptr = NULL;
      member.double_val_ = ::strtod(member.text_.ptr(), &endptr);
    }
  }
  return ret;
}

int ObFileJsonParser::parse_value(const char *data, int32_t len, int32_t &pos, ObFileJsonMember &member)
{
  int ret = OB_SUCCESS;
  skip_spaces(data, len, pos);
  if (pos >= len) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "invalid json value of the file table");
  } else if ('"' == data[pos]) {
    common::ObString text;
    if (OB_FAIL(parse_string(data, len, pos, text, *members_allocator_))) {
    } else {
      member.is_null_ = false;
      member.is_quoted_ = true;
      member.text_ = text;
    }
  } else if ('{' == data[pos] || '[' == data[pos]) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "nested json value is not supported by the file table");
  } else if ('t' == data[pos] && pos + 4 <= len && 0 == ::strncmp(data + pos, "true", 4)) {
    pos += 4;
    member.is_null_ = false;
    member.is_bool_ = true;
    member.bool_val_ = true;
    member.text_.assign_ptr(data + pos - 4, 4);
  } else if ('f' == data[pos] && pos + 5 <= len && 0 == ::strncmp(data + pos, "false", 5)) {
    pos += 5;
    member.is_null_ = false;
    member.is_bool_ = true;
    member.bool_val_ = false;
    member.text_.assign_ptr(data + pos - 5, 5);
  } else if ('n' == data[pos] && pos + 4 <= len && 0 == ::strncmp(data + pos, "null", 4)) {
    pos += 4;
    member.is_null_ = true;
    member.text_.assign_ptr(data + pos - 4, 4);
  } else {
    if (OB_FAIL(parse_number(data, len, pos, member))) {
    }
  }
  return ret;
}

int ObFileJsonParser::parse_object(const char *data, int32_t len, int32_t &pos, common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  skip_spaces(data, len, pos);
  if (pos >= len || '{' != data[pos]) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the line of the file table is not a json object");
  } else {
    ++pos;
    skip_spaces(data, len, pos);
    if (pos < len && '}' == data[pos]) {
      ++pos;
    } else {
      while (OB_SUCC(ret)) {
        skip_spaces(data, len, pos);
        common::ObString key;
        if (OB_FAIL(parse_string(data, len, pos, key, allocator))) {
          break;
        }
        skip_spaces(data, len, pos);
        if (pos >= len || ':' != data[pos]) {
          ret = OB_ERR_UNEXPECTED;
          LOG_USER_ERROR(OB_ERR_UNEXPECTED, "invalid json object of the file table");
          break;
        }
        ++pos;
        ObFileJsonMember member;
        if (OB_FAIL(parse_value(data, len, pos, member))) {
          break;
        }
        member.name_ = key;
        if (OB_FAIL(members_.push_back(member))) {
          break;
        }
        skip_spaces(data, len, pos);
        if (pos < len && ',' == data[pos]) {
          ++pos;
          continue;
        }
        if (pos < len && '}' == data[pos]) {
          ++pos;
          break;
        }
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "invalid json object of the file table");
        break;
      }
    }
  }
  return ret;
}

int ObFileJsonParser::parse(const common::ObString &line,
                            common::ObSEArray<ObFileJsonMember, 16> &members,
                            common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  members_.reset();
  members_allocator_ = &allocator;
  int32_t pos = 0;
  if (OB_FAIL(parse_object(line.ptr(), line.length(), pos, allocator))) {
  } else {
    if (OB_FAIL(members.assign(members_))) {
    }
  }
  return ret;
}

ObFileInferredType ObFileJsonParser::infer_type(const ObFileJsonMember &member)
{
  ObFileInferredType type = OB_FILE_TYPE_NULL;
  if (member.is_null_) {
    type = OB_FILE_TYPE_NULL;
  } else if (member.is_bool_) {
    type = OB_FILE_TYPE_BOOLEAN;
  } else if (member.is_int_) {
    type = OB_FILE_TYPE_BIGINT;
  } else if (member.is_quoted_) {
    if (member.text_.length() <= 0) {
      type = OB_FILE_TYPE_VARCHAR;
    } else {
      type = ObFileTypeUtil::infer_scalar_type(member.text_.ptr(), member.text_.length());
      if (OB_FILE_TYPE_NULL == type) {
        type = OB_FILE_TYPE_VARCHAR;
      }
    }
  } else {
    type = OB_FILE_TYPE_DOUBLE;
  }
  return type;
}

int ObFileJsonParser::convert_member(const ObFileJsonMember &member,
                                     ObFileInferredType type,
                                     ObFileValue &value)
{
  int ret = OB_SUCCESS;
  if (member.is_null_) {
    value.is_null_ = true;
    value.type_ = type;
  } else if (OB_FILE_TYPE_VARCHAR == type) {
    value.is_null_ = false;
    value.type_ = type;
    value.str_val_ = member.text_;
  } else if (OB_FILE_TYPE_BOOLEAN == type) {
    value.is_null_ = false;
    value.type_ = type;
    value.int_val_ = member.is_bool_ ? (member.bool_val_ ? 1 : 0) : 0;
  } else if (OB_FILE_TYPE_BIGINT == type) {
    value.is_null_ = false;
    value.type_ = type;
    value.int_val_ = member.is_int_ ? member.int_val_ : static_cast<int64_t>(member.double_val_);
  } else if (OB_FILE_TYPE_DOUBLE == type) {
    value.is_null_ = false;
    value.type_ = type;
    value.double_val_ = member.is_int_ ? static_cast<double>(member.int_val_) : member.double_val_;
  } else {
    /* the date and the datetime are stored as their text */
    ret = ObFileTypeUtil::convert_value(member.text_.ptr(),
                                         member.text_.length(),
                                         type,
                                         false,
                                         value);
  }
  return ret;
}

/* -------------------------------------------------------------------------- */
/* the schema inference                                                       */
/* -------------------------------------------------------------------------- */

int ObFileScanUtils::copy_string(common::ObIAllocator &allocator,
                                 const char *data,
                                 int32_t len,
                                 common::ObString &result)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(len > 0 ? len : 1));
  if (NULL == buf) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (len > 0 && NULL != data) {
      ::memcpy(buf, data, len);
    }
    result.assign_ptr(buf, len);
  }
  return ret;
}

int ObFileScanUtils::copy_string(common::ObIAllocator &allocator,
                                 const common::ObString &src,
                                 common::ObString &result)
{
  return copy_string(allocator, src.ptr(), src.length(), result);
}

static int copy_to_allocator(common::ObIAllocator &allocator,
                             const common::ObString &src,
                             common::ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(allocator.alloc(src.length() > 0 ? src.length() : 1));
  if (NULL == buf) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (src.length() > 0) {
      ::memcpy(buf, src.ptr(), src.length());
    }
    dst.assign_ptr(buf, src.length());
  }
  return ret;
}

int ObFileScanUtils::infer_csv_schema(common::ObIAllocator &allocator,
                                       const ObFileTableDescriptor &desc,
                                       ObSEArray<ObFileColumnSchema, 16> &columns,
                                       int64_t &file_size,
                                       int64_t &file_mtime)
{
  int ret = OB_SUCCESS;
  ObFileLineReader reader;
  ObFileCsvSplitter splitter(desc.options_.delimiter_, desc.options_.quote_char_, desc.options_.escape_char_);
  ObSEArray<ObFileCsvField, 16> fields;
  ObSEArray<ObFileColumnSchema, 16> cols;
  ObSEArray<ObFileInferredType, 16> types;
  ObSEArray<bool, 16> nullables;
  common::ObString record;
  int64_t record_no = 0;

  if (OB_FAIL(stat_file(desc.path_, file_size, file_mtime))) {
  } else if (OB_FAIL(reader.open(desc.path_))) {
  } else {
    reader.set_csv_params(desc.options_.quote_char_, desc.options_.escape_char_);
    if (OB_FAIL(reader.next_record(record, record_no))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file of the file table is empty");
    } else if (OB_FAIL(splitter.split(record, fields))) {
    } else if (fields.count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file of the file table is empty");
    } else {
      const int64_t column_count = fields.count();
      for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
        ObFileColumnSchema col;
        col.type_ = OB_FILE_TYPE_NULL;
        col.nullable_ = false;
        if (desc.options_.has_header_) {
          if (OB_FAIL(copy_to_allocator(allocator, fields.at(i).value_, col.name_))) {
          }
        } else {
          char tmp[64];
          const int32_t len = snprintf(tmp, sizeof(tmp), "column%ld", i);
          if (OB_FAIL(copy_to_allocator(allocator, common::ObString(len, tmp), col.name_))) {
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(cols.push_back(col))) {
        } else if (OB_SUCC(ret) && OB_FAIL(types.push_back(OB_FILE_TYPE_NULL))) {
        } else if (OB_SUCC(ret) && OB_FAIL(nullables.push_back(false))) {
        }
      }
      int64_t row_index = 0;
      while (OB_SUCC(ret) && row_index <= MAX_INFER_ROWS) {
        if (row_index > 0 || !desc.options_.has_header_) {
          for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
            const ObFileCsvField &field = fields.at(i);
            bool is_null = false;
            if (!field.is_quoted_) {
              if (desc.options_.nullstr_.length() <= 0) {
                is_null = (field.value_.length() <= 0);
              } else {
                is_null = (field.value_.length() == desc.options_.nullstr_.length()
                           && 0 == ::memcmp(field.value_.ptr(),
                                           desc.options_.nullstr_.ptr(),
                                           field.value_.length()));
              }
            }
            if (is_null) {
              nullables.at(i) = true;
            } else {
              ObFileInferredType type = OB_FILE_TYPE_VARCHAR;
              if (field.is_quoted_ && field.value_.length() <= 0) {
                type = OB_FILE_TYPE_VARCHAR;
              } else {
                type = ObFileTypeUtil::infer_scalar_type(field.value_.ptr(), field.value_.length());
              }
              types.at(i) = ObFileTypeUtil::merge_type(types.at(i), type);
            }
          }
        }
        ++row_index;
        if (OB_FAIL(reader.next_record(record, record_no))) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
          }
          break;
        }
        if (OB_FAIL(splitter.split(record, fields))) {
          break;
        }
        if (fields.count() != column_count) {
          ret = OB_ERR_UNEXPECTED;
          LOG_USER_ERROR(OB_ERR_UNEXPECTED,
                         "the number of the fields of the csv file does not match the header");
          break;
        }
      }
      if (OB_SUCC(ret)) {
        for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
          ObFileColumnSchema &col = cols.at(i);
          ObFileInferredType type = types.at(i);
          if (OB_FILE_TYPE_NULL == type) {
            type = OB_FILE_TYPE_VARCHAR;
          }
          col.type_ = type;
          col.nullable_ = nullables.at(i);
        }
        if (OB_SUCC(ret)) {
          uniquify_column_names(allocator, cols);
        }
        if (OB_SUCC(ret) && OB_FAIL(columns.assign(cols))) {
        }
      }
    }
  }
  return ret;
}

int ObFileScanUtils::infer_jsonl_schema(common::ObIAllocator &allocator,
                                         const ObFileTableDescriptor &desc,
                                         ObSEArray<ObFileColumnSchema, 16> &columns,
                                         int64_t &file_size,
                                         int64_t &file_mtime)
{
  int ret = OB_SUCCESS;
  ObFileLineReader reader;
  ObFileJsonParser parser;
  ObSEArray<ObFileJsonMember, 16> members;
  ObSEArray<ObFileColumnSchema, 16> cols;
  ObSEArray<ObFileInferredType, 16> types;
  ObSEArray<bool, 16> nullables;
  ObSEArray<int64_t, 16> seen_counts;
  /* the strings of one line are released before the next line is parsed */
  common::ObArenaAllocator line_allocator;
  common::ObString record;
  int64_t record_no = 0;
  int64_t line_count = 0;

  if (OB_FAIL(stat_file(desc.path_, file_size, file_mtime))) {
  } else if (OB_FAIL(reader.open(desc.path_))) {
  } else {
    reader.set_csv_params(0, 0);
    while (OB_SUCC(ret) && line_count <= MAX_INFER_ROWS) {
      if (OB_FAIL(reader.next_record(record, record_no))) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
        break;
      }
      if (record.length() <= 0) {
        continue;
      }
      line_allocator.reuse();
      if (OB_FAIL(parser.parse(record, members, line_allocator))) {
        break;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < members.count(); ++i) {
        const ObFileJsonMember &member = members.at(i);
        int64_t idx = -1;
        for (int64_t j = 0; j < cols.count(); ++j) {
          if (cols.at(j).name_.compare_equal(member.name_)) {
            idx = j;
            break;
          }
        }
        if (idx < 0) {
          ObFileColumnSchema col;
          col.type_ = OB_FILE_TYPE_NULL;
          col.nullable_ = true;
          if (OB_FAIL(copy_to_allocator(allocator, member.name_, col.name_))) {
            break;
          }
          if (OB_FAIL(cols.push_back(col))) {
            break;
          }
          if (OB_FAIL(types.push_back(OB_FILE_TYPE_NULL))) {
            break;
          }
          if (OB_FAIL(nullables.push_back(true))) {
            break;
          }
          if (OB_FAIL(seen_counts.push_back(0))) {
            break;
          }
          idx = cols.count() - 1;
        }
        ++seen_counts.at(idx);
        if (member.is_null_) {
          nullables.at(idx) = true;
        } else {
          const ObFileInferredType type = ObFileJsonParser::infer_type(member);
          types.at(idx) = ObFileTypeUtil::merge_type(types.at(idx), type);
        }
      }
      ++line_count;
    }
    if (OB_SUCC(ret)) {
      if (cols.count() <= 0) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the jsonl file of the file table is empty");
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
          ObFileColumnSchema &col = cols.at(i);
          ObFileInferredType type = types.at(i);
          if (OB_FILE_TYPE_NULL == type) {
            type = OB_FILE_TYPE_VARCHAR;
          }
          col.type_ = type;
          col.nullable_ = nullables.at(i) || (seen_counts.at(i) < line_count);
        }
        if (OB_SUCC(ret) && OB_FAIL(columns.assign(cols))) {
        }
      }
    }
  }
  return ret;
}

int ObFileScanUtils::infer_parquet_schema(common::ObIAllocator &allocator,
                                           const ObFileTableDescriptor &desc,
                                           ObSEArray<ObFileColumnSchema, 16> &columns,
                                           int64_t &file_size,
                                           int64_t &file_mtime)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(stat_file(desc.path_, file_size, file_mtime))) {
  } else {
    ObFileParquetReader reader;
    if (OB_FAIL(reader.init(desc.path_, allocator))) {
    } else if (OB_FAIL(columns.assign(reader.get_columns()))) {
    } else {
      /* the columns of the parquet file are unique already */
    }
  }
  return ret;
}

int ObFileScanUtils::infer_glob_schema(common::ObIAllocator &allocator,
                                        const ObFileTableDescriptor &desc,
                                        ObSEArray<ObFileColumnSchema, 16> &columns)
{
  int ret = OB_SUCCESS;
  ObFileColumnSchema col;
  col.type_ = OB_FILE_TYPE_VARCHAR;
  col.nullable_ = false;
  if (OB_FAIL(copy_to_allocator(allocator, common::ObString(4, "file"), col.name_))) {
  } else if (OB_FAIL(columns.push_back(col))) {
  }
  return ret;
}

/* -------------------------------------------------------------------------- */
/* the iterators                                                              */
/* -------------------------------------------------------------------------- */

int ObFileCsvIterator::init(const ObFileTableDescriptor &desc, common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  desc_ = desc;
  row_no_ = 0;
  splitter_ = ObFileCsvSplitter(desc.options_.delimiter_, desc.options_.quote_char_, desc.options_.escape_char_);
  if (OB_FAIL(reader_.open(desc.path_))) {
  } else {
    reader_.set_csv_params(desc.options_.quote_char_, desc.options_.escape_char_);
    if (desc.options_.has_header_) {
      /* skip the header record */
      common::ObString record;
      int64_t record_no = 0;
      if (OB_FAIL(reader_.next_record(record, record_no))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file of the file table is empty");
      }
    }
  }
  return ret;
}

int ObFileCsvIterator::next_row(ObFileValue *values, int64_t capacity, int64_t &row_count)
{
  int ret = OB_SUCCESS;
  row_count = 0;
  common::ObString record;
  int64_t record_no = 0;
  ObSEArray<ObFileCsvField, 16> fields;
  if (OB_FAIL(reader_.next_record(record, record_no))) {
    if (OB_ITER_END == ret) {
      ret = OB_ITER_END;
    }
  } else if (OB_FAIL(splitter_.split(record, fields))) {
  } else if (fields.count() > desc_.columns_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED,
                   "the number of the fields of the csv file does not match the header");
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < desc_.columns_.count() && i < capacity; ++i) {
      const ObFileColumnSchema &col = desc_.columns_.at(i);
      bool is_null = true;
      common::ObString text;
      if (i < fields.count()) {
        const ObFileCsvField &field = fields.at(i);
        text = field.value_;
        if (field.is_quoted_) {
          is_null = false;
        } else if (desc_.options_.nullstr_.length() <= 0) {
          is_null = (field.value_.length() <= 0);
        } else {
          is_null = (field.value_.length() == desc_.options_.nullstr_.length()
                     && 0 == ::memcmp(field.value_.ptr(),
                                     desc_.options_.nullstr_.ptr(),
                                     field.value_.length()));
        }
      }
      if (OB_FAIL(ObFileTypeUtil::convert_value(text.ptr(), text.length(), col.type_, is_null, values[i]))) {
        break;
      }
      ++row_count;
    }
    ++row_no_;
  }
  return ret;
}

int ObFileCsvIterator::reset()
{
  int ret = OB_SUCCESS;
  row_no_ = 0;
  if (OB_FAIL(reader_.open(desc_.path_))) {
  } else {
    reader_.set_csv_params(desc_.options_.quote_char_, desc_.options_.escape_char_);
    if (desc_.options_.has_header_) {
      common::ObString record;
      int64_t record_no = 0;
      if (OB_FAIL(reader_.next_record(record, record_no))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the csv file of the file table is empty");
      }
    }
  }
  return ret;
}

ObFileJsonlIterator::ObFileJsonlIterator()
    : desc_(), reader_(), parser_(), row_no_(0), row_allocator_()
{}

int ObFileJsonlIterator::init(const ObFileTableDescriptor &desc, common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  desc_ = desc;
  row_no_ = 0;
  if (OB_FAIL(reader_.open(desc.path_))) {
  } else {
    reader_.set_csv_params(0, 0);
  }
  return ret;
}

int ObFileJsonlIterator::next_row(ObFileValue *values, int64_t capacity, int64_t &row_count)
{
  int ret = OB_SUCCESS;
  row_count = 0;
  common::ObString record;
  int64_t record_no = 0;
  ObSEArray<ObFileJsonMember, 16> members;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(reader_.next_record(record, record_no))) {
      break;
    }
    if (record.length() <= 0) {
      continue;
    }
    row_allocator_.reuse();
    if (OB_FAIL(parser_.parse(record, members, row_allocator_))) {
      break;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < desc_.columns_.count() && i < capacity; ++i) {
      const ObFileColumnSchema &col = desc_.columns_.at(i);
      const ObFileJsonMember *found = NULL;
      for (int64_t j = 0; j < members.count(); ++j) {
        if (members.at(j).name_.compare_equal(col.name_)) {
          found = &members.at(j);
          break;
        }
      }
      if (NULL == found) {
        values[i].is_null_ = true;
        values[i].type_ = col.type_;
      } else if (OB_FAIL(ObFileJsonParser::convert_member(*found, col.type_, values[i]))) {
        break;
      }
      ++row_count;
    }
    ++row_no_;
    break;
  }
  return ret;
}

int ObFileJsonlIterator::reset()
{
  int ret = OB_SUCCESS;
  row_no_ = 0;
  if (OB_FAIL(reader_.open(desc_.path_))) {
  } else {
    reader_.set_csv_params(0, 0);
  }
  return ret;
}

int ObFileGlobIterator::init(const ObFileTableDescriptor &desc, common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  desc_ = desc;
  idx_ = 0;
  files_.reset();
  if (OB_FAIL(ObFileScanUtils::expand_glob(allocator, desc.path_, files_))) {
  }
  return ret;
}

int ObFileGlobIterator::next_row(ObFileValue *values, int64_t capacity, int64_t &row_count)
{
  int ret = OB_SUCCESS;
  row_count = 0;
  if (idx_ >= files_.count()) {
    ret = OB_ITER_END;
  } else if (capacity <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    values[0].is_null_ = false;
    values[0].type_ = OB_FILE_TYPE_VARCHAR;
    values[0].str_val_ = files_.at(idx_++);
    row_count = 1;
  }
  return ret;
}

int ObFileGlobIterator::reset()
{
  idx_ = 0;
  return OB_SUCCESS;
}

int ObFileScanUtils::create_iterator(common::ObIAllocator &allocator,
                                     const ObFileTableDescriptor &desc,
                                     ObFileScanIterator *&iterator)
{
  int ret = OB_SUCCESS;
  iterator = NULL;
  ObFileScanIterator *iter = NULL;
  switch (desc.format_) {
    case OB_FILE_FORMAT_CSV: {
      iter = static_cast<ObFileCsvIterator *>(allocator.alloc(sizeof(ObFileCsvIterator)));
      break;
    }
    case OB_FILE_FORMAT_JSONL: {
      iter = static_cast<ObFileJsonlIterator *>(allocator.alloc(sizeof(ObFileJsonlIterator)));
      break;
    }
    case OB_FILE_FORMAT_GLOB: {
      iter = static_cast<ObFileGlobIterator *>(allocator.alloc(sizeof(ObFileGlobIterator)));
      break;
    }
    case OB_FILE_FORMAT_PARQUET: {
      iter = static_cast<ObFileParquetIterator *>(allocator.alloc(sizeof(ObFileParquetIterator)));
      break;
    }
    default: {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "the format of the file table is not supported");
      break;
    }
  }
  if (OB_SUCC(ret) && NULL == iter) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  if (OB_SUCC(ret)) {
    switch (desc.format_) {
      case OB_FILE_FORMAT_CSV: {
        ObFileCsvIterator *csv_iter = new (iter) ObFileCsvIterator();
        if (OB_FAIL(csv_iter->init(desc, allocator))) {
        } else {
          iterator = csv_iter;
        }
        break;
      }
      case OB_FILE_FORMAT_JSONL: {
        ObFileJsonlIterator *jsonl_iter = new (iter) ObFileJsonlIterator();
        if (OB_FAIL(jsonl_iter->init(desc, allocator))) {
        } else {
          iterator = jsonl_iter;
        }
        break;
      }
      case OB_FILE_FORMAT_GLOB: {
        ObFileGlobIterator *glob_iter = new (iter) ObFileGlobIterator();
        if (OB_FAIL(glob_iter->init(desc, allocator))) {
        } else {
          iterator = glob_iter;
        }
        break;
      }
      case OB_FILE_FORMAT_PARQUET: {
        ObFileParquetIterator *parquet_iter = new (iter) ObFileParquetIterator();
        if (OB_FAIL(parquet_iter->init(desc, allocator))) {
        } else {
          iterator = parquet_iter;
        }
        break;
      }
      default: {
        break;
      }
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
