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

#include "sql/engine/basic/ob_file_parquet_reader.h"

#include "lib/utility/utility.h"
#include "share/ob_errno.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{
namespace
{

/* the parquet magic which is written at the beginning and at the end */
static const char PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};

/* the parquet physical types */
enum ObParquetType
{
  OB_PARQUET_BOOLEAN = 0,
  OB_PARQUET_INT32 = 1,
  OB_PARQUET_INT64 = 2,
  OB_PARQUET_INT96 = 3,
  OB_PARQUET_FLOAT = 4,
  OB_PARQUET_DOUBLE = 5,
  OB_PARQUET_BYTE_ARRAY = 6,
  OB_PARQUET_FIXED_LEN_BYTE_ARRAY = 7,
};

/* the parquet field repetition types */
enum ObParquetRepetition
{
  OB_PARQUET_REQUIRED = 0,
  OB_PARQUET_OPTIONAL = 1,
  OB_PARQUET_REPEATED = 2,
};

/* the parquet encodings */
enum ObParquetEncoding
{
  OB_PARQUET_ENCODING_PLAIN = 0,
  OB_PARQUET_ENCODING_PLAIN_DICTIONARY = 2,
  OB_PARQUET_ENCODING_RLE = 3,
  OB_PARQUET_ENCODING_BIT_PACKED = 4,
  OB_PARQUET_ENCODING_RLE_DICTIONARY = 8,
};

/* the parquet page types */
enum ObParquetPageType
{
  OB_PARQUET_DATA_PAGE = 0,
  OB_PARQUET_INDEX_PAGE = 1,
  OB_PARQUET_DICTIONARY_PAGE = 2,
  OB_PARQUET_DATA_PAGE_V2 = 3,
};

/* the parquet compression codecs */
enum ObParquetCodec
{
  OB_PARQUET_UNCOMPRESSED = 0,
  OB_PARQUET_SNAPPY = 1,
  OB_PARQUET_GZIP = 2,
};

/* the parquet converted types */
enum ObParquetConvertedType
{
  OB_PARQUET_CONVERTED_DATE = 6,
  OB_PARQUET_CONVERTED_TIMESTAMP_MILLIS = 9,
  OB_PARQUET_CONVERTED_TIMESTAMP_MICROS = 10,
};

/* the logical types of the thrift metadata */
enum ObParquetLogicalType
{
  OB_PARQUET_LOGICAL_NONE = 0,
  OB_PARQUET_LOGICAL_DATE = 1,
  OB_PARQUET_LOGICAL_TIMESTAMP = 2,
};

static const int64_t PARQUET_MAX_PAGE_SIZE = 64 * 1024 * 1024;
static const int64_t PARQUET_MAX_METADATA_SIZE = 64 * 1024 * 1024;

/* -------------------------------------------------------------------------- */
/* the thrift compact protocol                                                */
/* -------------------------------------------------------------------------- */

enum ObThriftCompactType
{
  OB_THRIFT_STOP = 0,
  OB_THRIFT_BOOL_TRUE = 1,
  OB_THRIFT_BOOL_FALSE = 2,
  OB_THRIFT_BYTE = 3,
  OB_THRIFT_I16 = 4,
  OB_THRIFT_I32 = 5,
  OB_THRIFT_I64 = 6,
  OB_THRIFT_DOUBLE = 7,
  OB_THRIFT_BINARY = 8,
  OB_THRIFT_LIST = 9,
  OB_THRIFT_SET = 10,
  OB_THRIFT_MAP = 11,
  OB_THRIFT_STRUCT = 12,
};

class ObThriftCompactReader
{
public:
  ObThriftCompactReader(const char *data, int64_t len)
      : data_(data), len_(len), pos_(0), last_field_id_(0), depth_(0) {}

  /**
   * Read the header of the next field of a struct. Returns false when the
   * end of the struct is reached.
   */
  bool read_field_begin(int16_t &field_id, int8_t &type)
  {
    if (pos_ >= len_) {
      return false;
    }
    const uint8_t byte = read_byte();
    if (OB_THRIFT_STOP == byte) {
      return false;
    }
    type = static_cast<int8_t>(byte & 0x0F);
    const int8_t delta = static_cast<int8_t>((byte & 0xF0) >> 4);
    if (0 == delta) {
      last_field_id_ = static_cast<int16_t>(read_zigzag());
    } else {
      last_field_id_ = static_cast<int16_t>(last_field_id_ + delta);
    }
    field_id = last_field_id_;
    return true;
  }

  int32_t read_i32() { return static_cast<int32_t>(read_zigzag()); }
  int64_t read_i64() { return read_zigzag(); }

  /* the returned pointer stays valid until the reader is destroyed */
  const char *read_binary(int32_t &length)
  {
    const uint64_t size = read_varint();
    if (pos_ + static_cast<int64_t>(size) > len_) {
      length = 0;
      pos_ = len_;
      return NULL;
    }
    length = static_cast<int32_t>(size);
    const char *ptr = data_ + pos_;
    pos_ += static_cast<int64_t>(size);
    return ptr;
  }

  bool read_list_begin(int8_t &elem_type, int32_t &size)
  {
    if (pos_ >= len_) {
      return false;
    }
    const uint8_t byte = read_byte();
    elem_type = static_cast<int8_t>(byte & 0x0F);
    uint32_t count = static_cast<uint32_t>((byte & 0xF0) >> 4);
    if (15 == count) {
      count = static_cast<uint32_t>(read_varint());
    }
    size = static_cast<int32_t>(count);
    return true;
  }

  /* skip the value of a field whose type is known */
  void skip(int8_t type)
  {
    switch (type) {
      case OB_THRIFT_BOOL_TRUE:
      case OB_THRIFT_BOOL_FALSE:
        break;
      case OB_THRIFT_BYTE:
        pos_ += 1;
        break;
      case OB_THRIFT_I16:
      case OB_THRIFT_I32:
      case OB_THRIFT_I64:
        read_zigzag();
        break;
      case OB_THRIFT_DOUBLE:
        pos_ += 8;
        break;
      case OB_THRIFT_BINARY: {
        int32_t length = 0;
        read_binary(length);
      } break;
      case OB_THRIFT_LIST:
      case OB_THRIFT_SET: {
        int8_t elem_type = 0;
        int32_t size = 0;
        if (read_list_begin(elem_type, size)) {
          for (int32_t i = 0; i < size; ++i) {
            skip(elem_type);
          }
        }
      } break;
      case OB_THRIFT_MAP: {
        const uint64_t size = read_varint();
        if (size > 0) {
          const uint8_t types = read_byte();
          const int8_t key_type = static_cast<int8_t>((types & 0xF0) >> 4);
          const int8_t value_type = static_cast<int8_t>(types & 0x0F);
          for (uint64_t i = 0; i < size; ++i) {
            skip(key_type);
            skip(value_type);
          }
        }
      } break;
      case OB_THRIFT_STRUCT: {
        int16_t field_id = 0;
        int8_t field_type = 0;
        push_struct();
        while (read_field_begin(field_id, field_type)) {
          skip(field_type);
        }
        pop_struct();
      } break;
      default:
        break;
    }
    if (pos_ > len_) {
      pos_ = len_;
    }
  }

  int64_t tell() const { return pos_; }

  /**
   * The field ids of a struct are relative to the previous field of the same
   * struct, the nested structs have their own context.
   */
  void push_struct()
  {
    if (depth_ < MAX_STRUCT_DEPTH) {
      field_id_stack_[depth_++] = last_field_id_;
      last_field_id_ = 0;
    }
  }

  void pop_struct()
  {
    if (depth_ > 0) {
      last_field_id_ = field_id_stack_[--depth_];
    }
  }

private:
  static const int32_t MAX_STRUCT_DEPTH = 32;

  uint8_t read_byte()
  {
    if (pos_ >= len_) {
      return 0;
    }
    return static_cast<uint8_t>(data_[pos_++]);
  }

  uint64_t read_varint()
  {
    uint64_t result = 0;
    int64_t shift = 0;
    while (pos_ < len_) {
      const uint8_t byte = read_byte();
      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
      if (shift > 63) {
        break;
      }
    }
    return result;
  }

  int64_t read_zigzag()
  {
    const uint64_t value = read_varint();
    return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
  }

  const char *data_;
  int64_t len_;
  int64_t pos_;
  int16_t last_field_id_;
  int16_t field_id_stack_[MAX_STRUCT_DEPTH];
  int32_t depth_;
};

/* -------------------------------------------------------------------------- */
/* the snappy decompression                                                    */
/* -------------------------------------------------------------------------- */

/**
 * Decompress a raw snappy block. The size of the uncompressed data is stored
 * at the beginning of the block.
 */
int ob_snappy_uncompress(const char *src, int64_t src_len, char *dst, int64_t dst_len)
{
  int ret = OB_SUCCESS;
  int64_t src_pos = 0;
  int64_t dst_pos = 0;
  uint64_t expected = 0;
  int64_t shift = 0;
  while (src_pos < src_len && shift < 35) {
    const uint8_t byte = static_cast<uint8_t>(src[src_pos++]);
    expected |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
  }
  if (static_cast<int64_t>(expected) != dst_len) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the snappy size mismatch", K(expected), K(dst_len));
  } else {
    while (src_pos < src_len) {
      const uint8_t tag = static_cast<uint8_t>(src[src_pos++]);
      const int64_t elem_type = tag & 0x03;
      if (0 == elem_type) {
        /* a literal */
        int64_t length = (tag >> 2) + 1;
        if (length > 60) {
          const int64_t extra = length - 60;
          if (src_pos + extra > src_len) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          int64_t value = 0;
          for (int64_t i = 0; i < extra; ++i) {
            value |= static_cast<int64_t>(static_cast<uint8_t>(src[src_pos + i])) << (8 * i);
          }
          src_pos += extra;
          length = value + 1;
        }
        if (src_pos + length > src_len || dst_pos + length > dst_len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        ::memcpy(dst + dst_pos, src + src_pos, length);
        src_pos += length;
        dst_pos += length;
      } else {
        /* a copy */
        int64_t length = 0;
        int64_t offset = 0;
        if (1 == elem_type) {
          length = 4 + ((tag >> 2) & 0x07);
          if (src_pos >= src_len) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          offset = ((static_cast<int64_t>(tag & 0xE0) << 3) | static_cast<uint8_t>(src[src_pos++]));
        } else if (2 == elem_type) {
          length = (tag >> 2) + 1;
          if (src_pos + 2 > src_len) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          offset = static_cast<uint8_t>(src[src_pos])
                   | (static_cast<int64_t>(static_cast<uint8_t>(src[src_pos + 1])) << 8);
          src_pos += 2;
        } else {
          length = (tag >> 2) + 1;
          if (src_pos + 4 > src_len) {
            ret = OB_ERR_UNEXPECTED;
            break;
          }
          for (int64_t i = 0; i < 4; ++i) {
            offset |= static_cast<int64_t>(static_cast<uint8_t>(src[src_pos + i])) << (8 * i);
          }
          src_pos += 4;
        }
        if (offset <= 0 || offset > dst_pos || dst_pos + length > dst_len) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        for (int64_t i = 0; i < length; ++i) {
          dst[dst_pos + i] = dst[dst_pos - offset + i];
        }
        dst_pos += length;
      }
    }
    if (OB_SUCC(ret) && dst_pos != dst_len) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the snappy block is incomplete", K(dst_pos), K(dst_len));
    }
  }
  return ret;
}

/* -------------------------------------------------------------------------- */
/* the rle and bit packed hybrid decoder                                       */
/* -------------------------------------------------------------------------- */

/**
 * Decode the levels which are encoded with the rle and bit packed hybrid
 * encoding. The number of the decoded levels is passed in count and the
 * number of the consumed bytes is returned in consumed.
 */
int ob_decode_hybrid_levels(const char *data,
                             int64_t len,
                             int32_t bit_width,
                             int32_t count,
                             common::ObSEArray<int32_t, 16> &levels,
                             int64_t &consumed)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  int32_t produced = 0;
  const uint32_t mask = (bit_width >= 32) ? 0xFFFFFFFFU : ((1U << bit_width) - 1);
  while (produced < count && pos < len) {
    /* the run header is an unsigned varint */
    uint64_t header = 0;
    int64_t shift = 0;
    while (pos < len && shift < 63) {
      const uint8_t byte = static_cast<uint8_t>(data[pos++]);
      header |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
    }
    if (0 == (header & 1)) {
      /* a run of the repeated value */
      const int32_t run_length = static_cast<int32_t>(header >> 1);
      const int32_t bytes = (bit_width + 7) / 8;
      uint64_t value = 0;
      for (int32_t i = 0; i < bytes; ++i) {
        if (pos < len) {
          value |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos++])) << (8 * i);
        }
      }
      value &= mask;
      for (int32_t i = 0; i < run_length && produced < count; ++i, ++produced) {
        if (OB_FAIL(levels.push_back(static_cast<int32_t>(value)))) {
          return ret;
        }
      }
    } else {
      /* a bit packed run of the groups of eight values */
      const int32_t groups = static_cast<int32_t>(header >> 1);
      const int32_t values = groups * 8;
      const int64_t bytes = static_cast<int64_t>(groups) * bit_width;
      if (pos + bytes > len) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the truncated bit packed run", K(pos), K(bytes), K(len));
        break;
      }
      for (int32_t i = 0; i < values && produced < count; ++i, ++produced) {
        const int64_t bit_index = static_cast<int64_t>(i) * bit_width;
        const int64_t byte_index = bit_index / 8;
        const int64_t bit_offset = bit_index % 8;
        uint64_t window = 0;
        const int32_t window_bytes = static_cast<int32_t>((bit_width + bit_offset + 7) / 8);
        for (int32_t k = 0; k < window_bytes; ++k) {
          window |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + byte_index + k])) << (8 * k);
        }
        const uint32_t level = static_cast<uint32_t>((window >> bit_offset) & mask);
        if (OB_FAIL(levels.push_back(static_cast<int32_t>(level)))) {
          return ret;
        }
      }
      pos += bytes;
    }
  }
  consumed = pos;
  if (OB_SUCC(ret) && produced < count) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the incomplete level section", K(produced), K(count));
  }
  return ret;
}

/* the number of the bits which are needed to store the values of [0, count) */
int32_t ob_parquet_bit_width(int64_t count)
{
  int32_t width = 0;
  while ((1LL << width) < count) {
    ++width;
  }
  return width;
}

/* -------------------------------------------------------------------------- */
/* the plain encoding                                                          */
/* -------------------------------------------------------------------------- */

int ob_decode_plain_values(const char *data,
                           int64_t len,
                           int32_t type,
                           int32_t type_length,
                           int32_t count,
                           common::ObIAllocator &allocator,
                           common::ObSEArray<ObFileValue, 16> &values)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  for (int32_t i = 0; i < count; ++i) {
    ObFileValue value;
    if (OB_PARQUET_BOOLEAN == type) {
      const int64_t bit_index = static_cast<int64_t>(i);
      const int64_t byte_index = bit_index / 8;
      if (byte_index >= len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const uint8_t byte = static_cast<uint8_t>(data[byte_index]);
      value.is_null_ = false;
      value.type_ = OB_FILE_TYPE_BOOLEAN;
      value.int_val_ = (byte >> (bit_index % 8)) & 0x01;
    } else if (OB_PARQUET_INT32 == type) {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      int32_t raw = 0;
      for (int32_t k = 0; k < 4; ++k) {
        raw |= static_cast<int32_t>(static_cast<uint8_t>(data[pos + k])) << (8 * k);
      }
      pos += 4;
      value.is_null_ = false;
      value.type_ = OB_FILE_TYPE_BIGINT;
      value.int_val_ = raw;
    } else if (OB_PARQUET_INT64 == type) {
      if (pos + 8 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      int64_t raw = 0;
      for (int32_t k = 0; k < 8; ++k) {
        raw |= static_cast<int64_t>(static_cast<uint8_t>(data[pos + k])) << (8 * k);
      }
      pos += 8;
      value.is_null_ = false;
      value.type_ = OB_FILE_TYPE_BIGINT;
      value.int_val_ = raw;
    } else if (OB_PARQUET_FLOAT == type) {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      float raw = 0.0f;
      uint32_t bits = 0;
      for (int32_t k = 0; k < 4; ++k) {
        bits |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + k])) << (8 * k);
      }
      pos += 4;
      ::memcpy(&raw, &bits, sizeof(raw));
      value.is_null_ = false;
      value.type_ = OB_FILE_TYPE_DOUBLE;
      value.double_val_ = static_cast<double>(raw);
    } else if (OB_PARQUET_DOUBLE == type) {
      if (pos + 8 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      double raw = 0.0;
      uint64_t bits = 0;
      for (int32_t k = 0; k < 8; ++k) {
        bits |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + k])) << (8 * k);
      }
      pos += 8;
      ::memcpy(&raw, &bits, sizeof(raw));
      value.is_null_ = false;
      value.type_ = OB_FILE_TYPE_DOUBLE;
      value.double_val_ = raw;
    } else if (OB_PARQUET_BYTE_ARRAY == type) {
      if (pos + 4 > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      uint32_t size = 0;
      for (int32_t k = 0; k < 4; ++k) {
        size |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + k])) << (8 * k);
      }
      pos += 4;
      if (pos + static_cast<int64_t>(size) > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      value.is_null_ = false;
      value.type_ = OB_FILE_TYPE_VARCHAR;
      if (OB_FAIL(ObFileScanUtils::copy_string(allocator, data + pos, size, value.str_val_))) {
        break;
      }
      pos += size;
    } else if (OB_PARQUET_FIXED_LEN_BYTE_ARRAY == type) {
      if (type_length <= 0 || pos + type_length > len) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      value.is_null_ = false;
      value.type_ = OB_FILE_TYPE_VARCHAR;
      if (OB_FAIL(ObFileScanUtils::copy_string(allocator, data + pos, type_length, value.str_val_))) {
        break;
      }
      pos += type_length;
    } else {
      /* the other types are not supported */
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "the parquet column type");
      break;
    }
    if (OB_SUCC(ret) && OB_FAIL(values.push_back(value))) {
      break;
    }
  }
  return ret;
}

} // namespace

/* -------------------------------------------------------------------------- */
/* the reader                                                                  */
/* -------------------------------------------------------------------------- */

ObFileParquetReader::ObFileParquetReader()
    : fd_(-1),
      file_size_(0),
      allocator_(NULL),
      columns_(),
      row_groups_(),
      column_values_(),
      schema_type_lengths_(),
      num_rows_(0),
      row_group_index_(-1),
      row_index_(0),
      loaded_(false)
{
}

ObFileParquetReader::~ObFileParquetReader()
{
  close();
}

int ObFileParquetReader::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  return OB_SUCCESS;
}

int ObFileParquetReader::read_at(int64_t offset, int64_t size, char *buf)
{
  int ret = OB_SUCCESS;
  int64_t done = 0;
  while (done < size) {
    const ssize_t n = ::pread(fd_, buf + done, static_cast<size_t>(size - done),
                               static_cast<off_t>(offset + done));
    if (n < 0) {
      if (EINTR == errno) {
        continue;
      }
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the failed read of the parquet file", K(errno), K(offset), K(size));
      break;
    } else if (0 == n) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the unexpected end of the parquet file", K(offset), K(size));
      break;
    }
    done += n;
  }
  return ret;
}

int ObFileParquetReader::init(const common::ObString &path, common::ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  allocator_ = &allocator;
  if (NULL == path.ptr() || path.length() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "the path of the parquet file");
    return ret;
  }
  /* the path is not null terminated, it comes from the encoded file descriptor */
  char *path_buf = static_cast<char *>(::malloc(path.length() + 1));
  if (NULL == path_buf) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ::memcpy(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    fd_ = ::open(path_buf, O_RDONLY);
    ::free(path_buf);
  }
  if (OB_SUCC(ret) && fd_ < 0) {
    ret = OB_FILE_NOT_EXIST;
    LOG_USER_ERROR(OB_FILE_NOT_EXIST, "the parquet file of the file table does not exist");
  } else if (OB_SUCC(ret)) {
    struct stat stat_buf;
    if (0 != ::fstat(fd_, &stat_buf)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      file_size_ = stat_buf.st_size;
    }
  }
  if (OB_SUCC(ret) && file_size_ < 12) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file is too short");
  }
  if (OB_SUCC(ret)) {
    char magic[4];
    if (OB_FAIL(read_at(0, 4, magic)) || 0 != ::memcmp(magic, PARQUET_MAGIC, 4)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file has an invalid magic");
    }
  }
  if (OB_SUCC(ret)) {
    char magic[4];
    if (OB_FAIL(read_at(file_size_ - 4, 4, magic)) || 0 != ::memcmp(magic, PARQUET_MAGIC, 4)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file has an invalid magic");
    }
  }
  if (OB_SUCC(ret)) {
    ret = load_file_metadata();
  }
  return ret;
}

int ObFileParquetReader::load_file_metadata()
{
  int ret = OB_SUCCESS;
  char footer_len_buf[4];
  int64_t footer_len = 0;
  int64_t footer_offset = 0;
  char *footer = NULL;
  ObThriftCompactReader *reader = NULL;
  common::ObSEArray<common::ObString, 16> schema_names;
  common::ObSEArray<int32_t, 16> schema_types;
  common::ObSEArray<int32_t, 16> schema_repetitions;
  common::ObSEArray<int32_t, 16> schema_logical_types;
  common::ObSEArray<int32_t, 16> schema_converted_types;
  common::ObSEArray<int32_t, 16> schema_type_lengths;
  int64_t root_children = 0;
  bool in_root = true;

  if (OB_FAIL(read_at(file_size_ - 8, 4, footer_len_buf))) {
  } else {
    for (int64_t i = 0; i < 4; ++i) {
      footer_len |= static_cast<int64_t>(static_cast<uint8_t>(footer_len_buf[i])) << (8 * i);
    }
    footer_offset = file_size_ - 8 - footer_len;
    if (footer_len <= 0 || footer_offset < 4 || footer_len > PARQUET_MAX_METADATA_SIZE) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet file has an invalid footer");
    }
  }
  if (OB_SUCC(ret)) {
    footer = static_cast<char *>(allocator_->alloc(footer_len));
    if (NULL == footer) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(read_at(footer_offset, footer_len, footer))) {
    }
  }
  if (OB_SUCC(ret)) {
    reader = new (allocator_->alloc(sizeof(ObThriftCompactReader))) ObThriftCompactReader(footer, footer_len);
    if (NULL == reader) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  /* the file metadata */
  if (OB_SUCC(ret)) {
    int16_t field_id = 0;
    int8_t type = 0;
    while (reader->read_field_begin(field_id, type)) {
      if (1 == field_id) {
        reader->read_i32();
      } else if (2 == field_id) {
        /* the schema */
        int8_t elem_type = 0;
        int32_t size = 0;
        if (!reader->read_list_begin(elem_type, size)) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        for (int32_t i = 0; i < size && OB_SUCC(ret); ++i) {
          int32_t node_type = -1;
          int32_t type_length = 0;
          int32_t repetition = -1;
          int32_t num_children = 0;
          int32_t converted_type = -1;
          int32_t logical_type = OB_PARQUET_LOGICAL_NONE;
          common::ObString name;
          int16_t sub_field_id = 0;
          int8_t sub_type = 0;
          reader->push_struct();
          while (reader->read_field_begin(sub_field_id, sub_type)) {
            if (1 == sub_field_id) {
              node_type = reader->read_i32();
            } else if (2 == sub_field_id) {
              type_length = reader->read_i32();
            } else if (3 == sub_field_id) {
              repetition = reader->read_i32();
            } else if (4 == sub_field_id) {
              int32_t name_len = 0;
              const char *name_ptr = reader->read_binary(name_len);
              name.assign_ptr(name_ptr, name_len);
            } else if (5 == sub_field_id) {
              num_children = reader->read_i32();
            } else if (6 == sub_field_id) {
              converted_type = reader->read_i32();
            } else if (8 == sub_field_id) {
              /* the logical type */
              int16_t logical_field_id = 0;
              int8_t logical_type_id = 0;
              reader->push_struct();
              while (reader->read_field_begin(logical_field_id, logical_type_id)) {
                if (6 == logical_field_id) {
                  logical_type = OB_PARQUET_LOGICAL_DATE;
                } else if (8 == logical_field_id) {
                  logical_type = OB_PARQUET_LOGICAL_TIMESTAMP;
                } else {
                  reader->skip(logical_type_id);
                }
              }
              reader->pop_struct();
            } else {
              reader->skip(sub_type);
            }
          }
          reader->pop_struct();
          if (in_root) {
            root_children = num_children;
            in_root = false;
          } else if (OB_FAIL(schema_names.push_back(name))
                     || OB_FAIL(schema_types.push_back(node_type))
                     || OB_FAIL(schema_repetitions.push_back(repetition))
                     || OB_FAIL(schema_logical_types.push_back(logical_type))
                     || OB_FAIL(schema_converted_types.push_back(converted_type))
                     || OB_FAIL(schema_type_lengths.push_back(type_length))) {
            break;
          }
        }
      } else if (3 == field_id) {
        num_rows_ = reader->read_i64();
      } else if (4 == field_id) {
        /* the row groups */
        int8_t elem_type = 0;
        int32_t size = 0;
        if (!reader->read_list_begin(elem_type, size)) {
          ret = OB_ERR_UNEXPECTED;
          break;
        }
        for (int32_t i = 0; i < size && OB_SUCC(ret); ++i) {
          ObParquetRowGroupMeta row_group;
          int16_t group_field_id = 0;
          int8_t group_type = 0;
          reader->push_struct();
          while (reader->read_field_begin(group_field_id, group_type)) {
            if (1 == group_field_id) {
              /* the column chunks */
              int8_t chunk_elem_type = 0;
              int32_t chunk_size = 0;
              if (!reader->read_list_begin(chunk_elem_type, chunk_size)) {
                ret = OB_ERR_UNEXPECTED;
                break;
              }
              for (int32_t j = 0; j < chunk_size && OB_SUCC(ret); ++j) {
                ObParquetColumnMeta column;
                int16_t chunk_field_id = 0;
                int8_t chunk_field_type = 0;
                reader->push_struct();
                while (reader->read_field_begin(chunk_field_id, chunk_field_type)) {
                  if (3 == chunk_field_id) {
                    /* the column metadata */
                    int16_t meta_field_id = 0;
                    int8_t meta_field_type = 0;
                    reader->push_struct();
                    while (reader->read_field_begin(meta_field_id, meta_field_type)) {
                      if (1 == meta_field_id) {
                        column.type_ = reader->read_i32();
                      } else if (2 == meta_field_id) {
                        /* the encodings */
                        int8_t enc_type = 0;
                        int32_t enc_size = 0;
                        if (reader->read_list_begin(enc_type, enc_size)) {
                          for (int32_t k = 0; k < enc_size; ++k) {
                            reader->read_i32();
                          }
                        }
                      } else if (3 == meta_field_id) {
                        /* the path in the schema */
                        int8_t path_type = 0;
                        int32_t path_size = 0;
                        if (reader->read_list_begin(path_type, path_size)) {
                          for (int32_t k = 0; k < path_size; ++k) {
                            int32_t len = 0;
                            reader->read_binary(len);
                          }
                        }
                      } else if (4 == meta_field_id) {
                        column.codec_ = reader->read_i32();
                      } else if (5 == meta_field_id) {
                        column.num_values_ = reader->read_i64();
                      } else if (9 == meta_field_id) {
                        column.data_page_offset_ = reader->read_i64();
                      } else if (11 == meta_field_id) {
                        column.dictionary_page_offset_ = reader->read_i64();
                      } else {
                        reader->skip(meta_field_type);
                      }
                    }
                    reader->pop_struct();
                  } else {
                    reader->skip(chunk_field_type);
                  }
                }
                reader->pop_struct();
                if (OB_SUCC(ret) && OB_FAIL(row_group.columns_.push_back(column))) {
                  break;
                }
              }
            } else if (3 == group_field_id) {
              row_group.num_rows_ = reader->read_i64();
            } else {
              reader->skip(group_type);
            }
          }
          reader->pop_struct();
          if (OB_SUCC(ret) && OB_FAIL(row_groups_.push_back(row_group))) {
            break;
          }
        }
      } else {
        reader->skip(type);
      }
    }
  }
  /* the columns of the schema */
  if (OB_SUCC(ret)) {
    if (root_children > 0 && static_cast<int64_t>(schema_names.count()) != root_children) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the parquet schema is not supported");
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 0; i < schema_names.count(); ++i) {
      ObFileColumnSchema column;
      const int32_t type = schema_types.at(i);
      const int32_t repetition = schema_repetitions.at(i);
      const int32_t logical_type = schema_logical_types.at(i);
      const int32_t converted_type = schema_converted_types.at(i);
      ObFileInferredType inferred_type = OB_FILE_TYPE_VARCHAR;
      if (OB_PARQUET_BOOLEAN == type) {
        inferred_type = OB_FILE_TYPE_BOOLEAN;
      } else if (OB_PARQUET_INT32 == type || OB_PARQUET_INT64 == type) {
        inferred_type = OB_FILE_TYPE_BIGINT;
      } else if (OB_PARQUET_FLOAT == type || OB_PARQUET_DOUBLE == type) {
        inferred_type = OB_FILE_TYPE_DOUBLE;
      } else if (OB_PARQUET_BYTE_ARRAY == type || OB_PARQUET_FIXED_LEN_BYTE_ARRAY == type) {
        if (OB_PARQUET_LOGICAL_DATE == logical_type || OB_PARQUET_CONVERTED_DATE == converted_type) {
          inferred_type = OB_FILE_TYPE_DATE;
        } else if (OB_PARQUET_LOGICAL_TIMESTAMP == logical_type
                   || OB_PARQUET_CONVERTED_TIMESTAMP_MILLIS == converted_type
                   || OB_PARQUET_CONVERTED_TIMESTAMP_MICROS == converted_type) {
          inferred_type = OB_FILE_TYPE_DATETIME;
        } else {
          inferred_type = OB_FILE_TYPE_VARCHAR;
        }
      }
      if (OB_FAIL(ObFileScanUtils::copy_string(*allocator_, schema_names.at(i), column.name_))) {
        break;
      }
      column.type_ = inferred_type;
      column.nullable_ = (OB_PARQUET_OPTIONAL == repetition);
      if (OB_FAIL(columns_.push_back(column))) {
        break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(schema_type_lengths_.assign(schema_type_lengths))) {
    } else {
      /* the type length belongs to the schema element of the column */
      for (int64_t i = 0; i < row_groups_.count(); ++i) {
        ObParquetRowGroupMeta &row_group = row_groups_.at(i);
        for (int64_t j = 0; j < row_group.columns_.count() && j < schema_type_lengths_.count(); ++j) {
          row_group.columns_.at(j).type_length_ = schema_type_lengths_.at(j);
        }
      }
    }
  }
  return ret;
}

int ObFileParquetReader::reset()
{
  row_group_index_ = -1;
  row_index_ = 0;
  loaded_ = false;
  column_values_.reset();
  return OB_SUCCESS;
}

int ObFileParquetReader::load_row_group(int64_t row_group_index)
{
  int ret = OB_SUCCESS;
  if (row_group_index < 0 || row_group_index >= row_groups_.count()) {
    ret = OB_ITER_END;
  } else {
    const ObParquetRowGroupMeta &row_group = row_groups_.at(row_group_index);
    column_values_.reset();
    for (int64_t i = 0; OB_SUCC(ret) && i < row_group.columns_.count(); ++i) {
      ObParquetColumnValues column;
      if (i < columns_.count()) {
        column.type_ = columns_.at(i).type_;
        column.nullable_ = columns_.at(i).nullable_;
      }
      if (OB_FAIL(decode_column_chunk(row_group.columns_.at(i), column, column.values_))) {
        break;
      }
      if (OB_FAIL(column_values_.push_back(column))) {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      row_group_index_ = row_group_index;
      row_index_ = 0;
      loaded_ = true;
    }
  }
  return ret;
}

int ObFileParquetReader::decode_column_chunk(const ObParquetColumnMeta &meta,
                                             const ObParquetColumnValues &column,
                                             common::ObSEArray<ObFileValue, 16> &values)
{
  int ret = OB_SUCCESS;
  int64_t offset = meta.data_page_offset_;
  if (OB_INVALID_ID != meta.dictionary_page_offset_ && meta.dictionary_page_offset_ < offset) {
    offset = meta.dictionary_page_offset_;
  }
  int64_t produced = 0;
  common::ObSEArray<ObFileValue, 16> dictionary;
  const int32_t max_def_level = column.nullable_ ? 1 : 0;

  while (OB_SUCC(ret) && produced < meta.num_values_) {
    /* read the page header */
    char *header_buf = static_cast<char *>(allocator_->alloc(4096));
    if (NULL == header_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      break;
    }
    int64_t header_len = 0;
    while (header_len < 4096) {
      const ssize_t n = ::pread(fd_, header_buf + header_len, 4096 - header_len,
                                static_cast<off_t>(offset + header_len));
      if (n <= 0) {
        break;
      }
      header_len += n;
    }
    if (header_len <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the unexpected end of the parquet file", K(offset));
      break;
    }
    ObThriftCompactReader reader(header_buf, header_len);
    int32_t page_type = -1;
    int32_t uncompressed_size = 0;
    int32_t compressed_size = 0;
    int32_t num_values = 0;
    int32_t encoding = OB_PARQUET_ENCODING_PLAIN;
    int32_t def_levels_len = 0;
    int32_t rep_levels_len = 0;
    bool is_compressed = true;
    int16_t field_id = 0;
    int8_t type = 0;
    while (reader.read_field_begin(field_id, type)) {
      if (1 == field_id) {
        page_type = reader.read_i32();
      } else if (2 == field_id) {
        uncompressed_size = reader.read_i32();
      } else if (3 == field_id) {
        compressed_size = reader.read_i32();
      } else if (5 == field_id) {
        /* the data page header of the version one pages */
        int16_t page_field_id = 0;
        int8_t page_type_id = 0;
        reader.push_struct();
        while (reader.read_field_begin(page_field_id, page_type_id)) {
          if (1 == page_field_id) {
            num_values = reader.read_i32();
          } else if (2 == page_field_id) {
            encoding = reader.read_i32();
          } else {
            reader.skip(page_type_id);
          }
        }
        reader.pop_struct();
      } else if (7 == field_id) {
        /* the dictionary page header */
        int16_t page_field_id = 0;
        int8_t page_type_id = 0;
        reader.push_struct();
        while (reader.read_field_begin(page_field_id, page_type_id)) {
          if (1 == page_field_id) {
            num_values = reader.read_i32();
          } else if (2 == page_field_id) {
            encoding = reader.read_i32();
          } else {
            reader.skip(page_type_id);
          }
        }
        reader.pop_struct();
      } else if (8 == field_id) {
        /* the data page header of the version two pages */
        int16_t page_field_id = 0;
        int8_t page_type_id = 0;
        reader.push_struct();
        while (reader.read_field_begin(page_field_id, page_type_id)) {
          if (1 == page_field_id) {
            num_values = reader.read_i32();
          } else if (2 == page_field_id) {
            /* the number of the nulls */
            reader.read_i32();
          } else if (3 == page_field_id) {
            /* the number of the rows */
            reader.read_i32();
          } else if (4 == page_field_id) {
            encoding = reader.read_i32();
          } else if (5 == page_field_id) {
            def_levels_len = reader.read_i32();
          } else if (6 == page_field_id) {
            rep_levels_len = reader.read_i32();
          } else if (7 == page_field_id) {
            /* the value of a boolean is stored in the type of the field header */
            is_compressed = (OB_THRIFT_BOOL_FALSE != page_type_id);
          } else {
            reader.skip(page_type_id);
          }
        }
        reader.pop_struct();
      } else {
        reader.skip(type);
      }
    }
    const int64_t page_data_offset = offset + reader.tell();
    if (compressed_size <= 0 || uncompressed_size < 0
        || compressed_size > PARQUET_MAX_PAGE_SIZE || uncompressed_size > PARQUET_MAX_PAGE_SIZE) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the invalid parquet page", K(page_type), K(compressed_size), K(uncompressed_size));
      break;
    }
    /* read the data of the page */
    char *page_data = static_cast<char *>(allocator_->alloc(compressed_size));
    if (NULL == page_data) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      break;
    }
    if (OB_FAIL(read_at(page_data_offset, compressed_size, page_data))) {
      break;
    }
    offset = page_data_offset + compressed_size;

    if (OB_PARQUET_DICTIONARY_PAGE == page_type) {
      /* the dictionary page holds the values which are referenced by the indices */
      char *plain_data = page_data;
      int64_t plain_len = compressed_size;
      if (meta.codec_ != OB_PARQUET_UNCOMPRESSED) {
        char *buf = static_cast<char *>(allocator_->alloc(uncompressed_size > 0 ? uncompressed_size : 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          break;
        }
        if (OB_FAIL(decompress_page(meta.codec_, page_data, compressed_size, uncompressed_size, buf))) {
          break;
        }
        plain_data = buf;
        plain_len = uncompressed_size;
      }
      ret = ob_decode_plain_values(plain_data, plain_len, meta.type_, meta.type_length_,
                                   num_values, *allocator_, dictionary);
    } else if (OB_PARQUET_DATA_PAGE == page_type || OB_PARQUET_DATA_PAGE_V2 == page_type) {
      ret = decode_page_values(meta, column, page_type, num_values, encoding,
                                page_data, compressed_size, uncompressed_size,
                                def_levels_len, rep_levels_len, is_compressed,
                                dictionary, values);
      if (OB_SUCC(ret)) {
        produced += num_values;
      }
    } else {
      /* the index page is not needed */
      break;
    }
  }
  return ret;
}

int ObFileParquetReader::decompress_page(int32_t codec,
                                         const char *src,
                                         int64_t src_len,
                                         int64_t dst_len,
                                         char *dst)
{
  int ret = OB_SUCCESS;
  if (OB_PARQUET_UNCOMPRESSED == codec) {
    if (src_len != dst_len) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ::memcpy(dst, src, dst_len);
    }
  } else if (OB_PARQUET_SNAPPY == codec) {
    ret = ob_snappy_uncompress(src, src_len, dst, dst_len);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "the compression codec of the parquet file");
  }
  return ret;
}

int ObFileParquetReader::decode_page_values(const ObParquetColumnMeta &meta,
                                            const ObParquetColumnValues &column,
                                            int32_t page_type,
                                            int32_t num_values,
                                            int32_t encoding,
                                            const char *data,
                                            int64_t data_len,
                                            int64_t uncompressed_size,
                                            int32_t def_levels_len,
                                            int32_t rep_levels_len,
                                            bool is_compressed,
                                            const common::ObSEArray<ObFileValue, 16> &dictionary,
                                            common::ObSEArray<ObFileValue, 16> &values)
{
  int ret = OB_SUCCESS;
  const int32_t max_def_level = column.nullable_ ? 1 : 0;
  const int32_t def_bit_width = ob_parquet_bit_width(max_def_level + 1);
  common::ObSEArray<int32_t, 16> def_levels;
  const char *values_data = data;
  int64_t values_len = data_len;

  if (OB_PARQUET_DATA_PAGE_V2 == page_type) {
    /* the repetition levels come first and the definition levels follow them */
    if (rep_levels_len + def_levels_len > data_len) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the invalid level section", K(rep_levels_len), K(def_levels_len), K(data_len));
    } else {
      values_data = data + rep_levels_len + def_levels_len;
      values_len = data_len - rep_levels_len - def_levels_len;
      if (max_def_level > 0) {
        int64_t consumed = 0;
        ret = ob_decode_hybrid_levels(data + rep_levels_len, def_levels_len, def_bit_width,
                                      num_values, def_levels, consumed);
      }
      if (OB_SUCC(ret) && is_compressed && meta.codec_ != OB_PARQUET_UNCOMPRESSED) {
        const int64_t plain_len = uncompressed_size - rep_levels_len - def_levels_len;
        char *buf = static_cast<char *>(allocator_->alloc(plain_len > 0 ? plain_len : 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(decompress_page(meta.codec_, values_data, values_len, plain_len, buf))) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          values_data = buf;
          values_len = plain_len;
        }
      }
    }
  } else {
    /* the whole page is compressed in the version one pages */
    char *plain_data = const_cast<char *>(data);
    int64_t plain_len = data_len;
    if (meta.codec_ != OB_PARQUET_UNCOMPRESSED) {
      char *buf = static_cast<char *>(allocator_->alloc(uncompressed_size > 0 ? uncompressed_size : 1));
      if (NULL == buf) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(decompress_page(meta.codec_, data, data_len, uncompressed_size, buf))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        plain_data = buf;
        plain_len = uncompressed_size;
      }
    }
    if (OB_SUCC(ret)) {
      int64_t consumed = 0;
      if (max_def_level > 0) {
        ret = ob_decode_hybrid_levels(plain_data, plain_len, def_bit_width, num_values,
                                      def_levels, consumed);
      }
      if (OB_SUCC(ret)) {
        values_data = plain_data + consumed;
        values_len = plain_len - consumed;
      }
    }
  }

  if (OB_SUCC(ret)) {
    /* the number of the values which are not null */
    int32_t valid_values = num_values;
    for (int32_t i = 0; i < def_levels.count(); ++i) {
      if (def_levels.at(i) < max_def_level) {
        --valid_values;
      }
    }
    if (OB_PARQUET_ENCODING_PLAIN_DICTIONARY == encoding
        || OB_PARQUET_ENCODING_RLE_DICTIONARY == encoding) {
      /* the values are the indices of the dictionary */
      if (values_len < 1) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const int32_t bit_width = static_cast<uint8_t>(values_data[0]);
        common::ObSEArray<int32_t, 16> indices;
        int64_t consumed = 0;
        ret = ob_decode_hybrid_levels(values_data + 1, values_len - 1, bit_width,
                                      valid_values, indices, consumed);
        if (OB_SUCC(ret)) {
          for (int32_t i = 0; i < indices.count(); ++i) {
            const int32_t index = indices.at(i);
            if (index < 0 || index >= dictionary.count()) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("the invalid dictionary index", K(index), K(dictionary.count()));
              break;
            }
            if (OB_FAIL(values.push_back(dictionary.at(index)))) {
              break;
            }
          }
        }
      }
    } else if (OB_PARQUET_ENCODING_PLAIN == encoding) {
      common::ObSEArray<ObFileValue, 16> plain_values;
      ret = ob_decode_plain_values(values_data, values_len, meta.type_, meta.type_length_,
                                   valid_values, *allocator_, plain_values);
      if (OB_SUCC(ret)) {
        /* the null values are interleaved with the values which are not null */
        int32_t value_index = 0;
        for (int32_t i = 0; i < num_values; ++i) {
          ObFileValue value;
          if (i < def_levels.count() && def_levels.at(i) < max_def_level) {
            value.is_null_ = true;
            value.type_ = column.type_;
          } else if (value_index < plain_values.count()) {
            value = plain_values.at(value_index++);
          }
          if (OB_SUCC(ret) && OB_FAIL(values.push_back(value))) {
            break;
          }
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "the encoding of the parquet file");
    }
  }
  return ret;
}

int ObFileParquetReader::next_row(ObFileValue *values, int64_t capacity, int64_t &row_count)
{
  int ret = OB_SUCCESS;
  row_count = 0;
  while (true) {
    if (!loaded_) {
      ret = load_row_group(row_group_index_ + 1);
      if (OB_ITER_END == ret) {
        break;
      } else if (OB_FAIL(ret)) {
        break;
      }
    }
    const ObParquetRowGroupMeta &row_group = row_groups_.at(row_group_index_);
    if (row_index_ >= row_group.num_rows_) {
      loaded_ = false;
      continue;
    }
    const int64_t count = column_values_.count();
    const int64_t emit = (capacity < count) ? capacity : count;
    for (int64_t i = 0; i < emit; ++i) {
      const ObParquetColumnValues &column = column_values_.at(i);
      if (row_index_ < column.values_.count()) {
        values[i] = column.values_.at(row_index_);
      } else {
        values[i] = ObFileValue();
      }
    }
    ++row_index_;
    row_count = emit;
    break;
  }
  return ret;
}

int ObFileParquetIterator::init(const ObFileTableDescriptor &desc, common::ObIAllocator &allocator)
{
  return reader_.init(desc.path_, allocator);
}

int ObFileParquetIterator::next_row(ObFileValue *values, int64_t capacity, int64_t &row_count)
{
  return reader_.next_row(values, capacity, row_count);
}

int ObFileParquetIterator::reset()
{
  return reader_.reset();
}

} // end namespace sql
} // end namespace oceanbase
