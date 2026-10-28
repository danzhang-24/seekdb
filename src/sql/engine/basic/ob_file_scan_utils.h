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

#ifndef SRC_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
#define SRC_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_

#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace sql
{

/**
 * The physical file formats which can be scanned by the file table functions.
 */
enum ObFileTableFormat
{
  OB_FILE_FORMAT_CSV = 0,
  OB_FILE_FORMAT_JSONL = 1,
  OB_FILE_FORMAT_PARQUET = 2,
  OB_FILE_FORMAT_GLOB = 3,
};

/**
 * The type lattice used by the deterministic type inference.
 *
 *   NULL    < BOOLEAN < BIGINT < DOUBLE
 *   NULL    < DATE    < DATETIME
 *   VARCHAR is the top of the lattice, it absorbs every other type.
 */
enum ObFileInferredType
{
  OB_FILE_TYPE_NULL = 0,
  OB_FILE_TYPE_BOOLEAN = 1,
  OB_FILE_TYPE_BIGINT = 2,
  OB_FILE_TYPE_DOUBLE = 3,
  OB_FILE_TYPE_DATE = 4,
  OB_FILE_TYPE_DATETIME = 5,
  OB_FILE_TYPE_VARCHAR = 6,
};

class ObFileFormatUtil
{
public:
  static const char *CSV;
  static const char *JSONL;
  static const char *PARQUET;
  static const char *GLOB;
};

/**
 * One cell of a scanned row. Only the member matching the column type is valid.
 */
struct ObFileValue
{
  ObFileValue() : is_null_(true), type_(OB_FILE_TYPE_NULL), int_val_(0), double_val_(0.0), str_val_() {}
  bool is_null_;
  ObFileInferredType type_;
  int64_t int_val_;
  double double_val_;
  common::ObString str_val_;
};

class ObFileTypeUtil
{
public:
  /* the display name of an inferred type, e.g. BIGINT / DOUBLE / BOOLEAN ... */
  static const char *type_name(ObFileInferredType type);
  /* merge two inferred types, the result is the narrowest type which can hold both */
  static ObFileInferredType merge_type(ObFileInferredType lhs, ObFileInferredType rhs);
  /* the sql object type used to store the value of an inferred type */
  static ObObjType obj_type(ObFileInferredType type);
  /* infer the type of one scalar cell */
  static ObFileInferredType infer_scalar_type(const char *data, int32_t len);
  /* parse a date cell, only "YYYY-MM-DD" is accepted */
  static bool parse_date(const char *data, int32_t len, int32_t &date_value);
  /* parse a datetime cell, "YYYY-MM-DD[ T]HH:MM:SS[.ffffff]" is accepted */
  static bool parse_datetime(const char *data, int32_t len, int64_t &datetime_value);
  /**
   * Convert the textual representation of a cell into the value of the given
   * type. The value is NULL when the text is empty and the type is not VARCHAR.
   */
  static int convert_value(const char *data,
                           int32_t len,
                           ObFileInferredType type,
                           bool is_null_text,
                           ObFileValue &value);
};

struct ObFileColumnSchema
{
  ObFileColumnSchema() : type_(OB_FILE_TYPE_VARCHAR), nullable_(true) {}
  ObString name_;
  ObFileInferredType type_;
  bool nullable_;
};

struct ObFileTableOptions
{
  ObFileTableOptions()
      : has_header_(true),
        delimiter_(','),
        quote_char_('"'),
        escape_char_('"'),
        sample_size_(-1),
        nullstr_(),
        format_option_()
  {}
  bool has_header_;
  char delimiter_;
  char quote_char_;
  char escape_char_;
  int64_t sample_size_;
  ObString nullstr_;
  ObString format_option_;
};

/**
 * The complete description of one file table. It is produced while resolving
 * the table function and is handed over to the executor through the plan.
 */
class ObFileTableDescriptor
{
public:
  ObFileTableDescriptor()
      : format_(OB_FILE_FORMAT_CSV), file_size_(0), file_mtime_(0) {}
  ObFileTableFormat format_;
  ObString path_;
  ObFileTableOptions options_;
  ObSEArray<ObFileColumnSchema, 16> columns_;
  int64_t file_size_;
  int64_t file_mtime_;

  /**
   * Serialize the descriptor into a binary safe string so that it can be
   * carried by the plan and rebuilt inside the executor.
   */
  int encode(common::ObIAllocator &allocator, common::ObString &result) const;
  static int decode(common::ObIAllocator &allocator, const common::ObString &str, ObFileTableDescriptor &desc);
  /* the number of columns described by the descriptor */
  int64_t column_count() const { return columns_.count(); }
  /* check whether the string carries an encoded file table descriptor */
  static bool is_file_descriptor(const common::ObString &str);
};

class ObFileScanIterator
{
public:
  virtual ~ObFileScanIterator() {}
  /**
   * Read the next row of the file. Returns OB_ITER_END when the file is
   * exhausted. The values are written into the given array, the number of
   * produced columns is returned in row_count.
   */
  virtual int next_row(ObFileValue *values, int64_t capacity, int64_t &row_count) = 0;
  /* restart the scan, used when the operator is rescanned */
  virtual int reset() = 0;
};

class ObFileScanUtils
{
public:
  /* the maximum size of a file which may be scanned */
  static const int64_t MAX_FILE_SIZE = 512LL * 1024 * 1024;
  /* the maximum number of files which may be returned by a glob table function */
  static const int64_t MAX_GLOB_FILES = 100000;
  /* the maximum number of rows which are inspected while inferring a schema */
  static const int64_t MAX_INFER_ROWS = 1000000;

  /* check the suffix of the path and return the matching format */
  static int check_file_format(const common::ObString &path, ObFileTableFormat &format);
  /* stat the file and record its size and modification time */
  static int stat_file(const common::ObString &path, int64_t &file_size, int64_t &file_mtime);
  /* expand the glob pattern into the list of matching files */
  static int expand_glob(common::ObIAllocator &allocator,
                         const common::ObString &pattern,
                         common::ObSEArray<common::ObString, 16> &files);
  /* infer the schema of a csv file */
  static int infer_csv_schema(common::ObIAllocator &allocator,
                              const ObFileTableDescriptor &desc,
                              ObSEArray<ObFileColumnSchema, 16> &columns,
                              int64_t &file_size,
                              int64_t &file_mtime);
  /* infer the schema of a jsonl file */
  static int infer_jsonl_schema(common::ObIAllocator &allocator,
                                const ObFileTableDescriptor &desc,
                                ObSEArray<ObFileColumnSchema, 16> &columns,
                                int64_t &file_size,
                                int64_t &file_mtime);
  /* infer the schema of a parquet file */
  static int infer_parquet_schema(common::ObIAllocator &allocator,
                                  const ObFileTableDescriptor &desc,
                                  ObSEArray<ObFileColumnSchema, 16> &columns,
                                  int64_t &file_size,
                                  int64_t &file_mtime);
  /* infer the schema of a glob table function, it always has one column */
  static int infer_glob_schema(common::ObIAllocator &allocator,
                               const ObFileTableDescriptor &desc,
                               ObSEArray<ObFileColumnSchema, 16> &columns);
  /* build the iterator which produces the rows of the file */
  static int create_iterator(common::ObIAllocator &allocator,
                             const ObFileTableDescriptor &desc,
                             ObFileScanIterator *&iterator);
  /* make the column names unique by appending "__2", "__3" ... */
  static void uniquify_column_names(common::ObIAllocator &allocator,
                                     ObSEArray<ObFileColumnSchema, 16> &columns);
  /* copy a string into the given allocator */
  static int copy_string(common::ObIAllocator &allocator,
                         const char *data,
                         int32_t len,
                         common::ObString &result);
  static int copy_string(common::ObIAllocator &allocator,
                         const common::ObString &src,
                         common::ObString &result);
};

/**
 * A buffered reader on top of a local file. It never holds the whole file in
 * memory, the data is fetched chunk by chunk. When a quote character is
 * configured the reader returns csv records, so a record may span several
 * physical lines.
 */
class ObFileLineReader
{
public:
  ObFileLineReader();
  ~ObFileLineReader();
  int open(const common::ObString &path);
  void close();
  /* configure the csv record boundary, quote_char == 0 means physical lines */
  void set_csv_params(char quote_char, char escape_char);
  /* read the next record, the returned string stays valid until the next call */
  int next_record(common::ObString &record, int64_t &record_no);
  bool is_eof() const { return eof_ && buf_pos_ >= buf_len_ && rec_len_ == 0; }
  int64_t get_file_size() const { return file_size_; }

private:
  int ensure_buffer(int64_t extra);
  int compact_buffer();
  int read_more();
  int append_char(char c);
  int append_bytes(const char *data, int64_t len);

  int fd_;
  int64_t file_size_;
  char *buf_;
  int64_t buf_cap_;
  int64_t buf_len_;
  int64_t buf_pos_;
  bool eof_;
  int64_t record_no_;
  char quote_char_;
  char escape_char_;
  /* the state which may be split by a chunk boundary */
  bool in_quote_;
  bool pending_quote_;
  bool pending_escape_;
  bool skip_lf_;
  /* the buffer which holds the record being assembled */
  char *rec_buf_;
  int64_t rec_cap_;
  int64_t rec_len_;
};

/**
 * One field of a csv record. The value points into the buffer of the splitter
 * and stays valid until the next split call.
 */
struct ObFileCsvField
{
  ObFileCsvField() : value_(), is_quoted_(false) {}
  common::ObString value_;
  bool is_quoted_;
};

/**
 * Split one csv record into its fields.
 */
class ObFileCsvSplitter
{
public:
  ObFileCsvSplitter(char delimiter, char quote_char, char escape_char);
  ~ObFileCsvSplitter();
  int split(const common::ObString &record, common::ObSEArray<ObFileCsvField, 16> &fields);

private:
  int append(const char *data, int32_t len);
  int ensure_capacity(int64_t capacity);
  char delimiter_;
  char quote_char_;
  char escape_char_;
  char *buf_;
  int64_t buf_cap_;
  int64_t buf_len_;
};

/**
 * One member of a flat json object.
 */
struct ObFileJsonMember
{
  ObFileJsonMember()
      : is_null_(true), is_quoted_(false), is_int_(false), is_bool_(false), bool_val_(false),
        int_val_(0), double_val_(0.0), name_(), text_() {}
  bool is_null_;
  /* the value is a json string, the text holds the unescaped content */
  bool is_quoted_;
  /* the value is a json number without a fraction or an exponent */
  bool is_int_;
  bool is_bool_;
  bool bool_val_;
  int64_t int_val_;
  double double_val_;
  /* the name of the member */
  common::ObString name_;
  /* the raw text of a string, a number or a boolean */
  common::ObString text_;
};

/**
 * Parse one flat json object. Nested objects and arrays are rejected.
 */
class ObFileJsonParser
{
public:
  ObFileJsonParser() : members_() {}
  ~ObFileJsonParser();
  /**
   * Parse one flat json object. Nested objects and arrays are rejected.
   * The strings of the members are allocated from the given allocator, which
   * must outlive the returned members.
   */
  int parse(const common::ObString &line,
            common::ObSEArray<ObFileJsonMember, 16> &members,
            common::ObIAllocator &allocator);
  /* the inferred type of one json member */
  static ObFileInferredType infer_type(const ObFileJsonMember &member);
  /* convert one json member into the value of the given type */
  static int convert_member(const ObFileJsonMember &member,
                            ObFileInferredType type,
                            ObFileValue &value);

private:
  int parse_object(const char *data, int32_t len, int32_t &pos, common::ObIAllocator &allocator);
  int parse_string(const char *data, int32_t len, int32_t &pos, common::ObString &result, common::ObIAllocator &allocator);
  int parse_number(const char *data, int32_t len, int32_t &pos, ObFileJsonMember &member);
  int parse_value(const char *data, int32_t len, int32_t &pos, ObFileJsonMember &member);

  common::ObSEArray<ObFileJsonMember, 16> members_;
  /* the allocator which owns the strings of the members */
  common::ObIAllocator *members_allocator_;
};

/**
 * The iterator which scans a csv file.
 */
class ObFileCsvIterator : public ObFileScanIterator
{
public:
  ObFileCsvIterator() : desc_(), reader_(), splitter_(',', '"', '"'), row_no_(0) {}
  int init(const ObFileTableDescriptor &desc, common::ObIAllocator &allocator);
  int next_row(ObFileValue *values, int64_t capacity, int64_t &row_count) override;
  int reset() override;

private:
  ObFileTableDescriptor desc_;
  ObFileLineReader reader_;
  ObFileCsvSplitter splitter_;
  int64_t row_no_;
};

/**
 * The iterator which scans a json lines file.
 */
class ObFileJsonlIterator : public ObFileScanIterator
{
public:
  ObFileJsonlIterator();
  int init(const ObFileTableDescriptor &desc, common::ObIAllocator &allocator);
  int next_row(ObFileValue *values, int64_t capacity, int64_t &row_count) override;
  int reset() override;

private:
  ObFileTableDescriptor desc_;
  ObFileLineReader reader_;
  ObFileJsonParser parser_;
  int64_t row_no_;
  /* the strings of the current row are allocated in this arena */
  common::ObArenaAllocator row_allocator_;
};

/**
 * The iterator which returns the files matched by a glob pattern.
 */
class ObFileGlobIterator : public ObFileScanIterator
{
public:
  ObFileGlobIterator() : desc_(), files_(), idx_(0) {}
  int init(const ObFileTableDescriptor &desc, common::ObIAllocator &allocator);
  int next_row(ObFileValue *values, int64_t capacity, int64_t &row_count) override;
  int reset() override;

private:
  ObFileTableDescriptor desc_;
  common::ObSEArray<common::ObString, 16> files_;
  int64_t idx_;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* SRC_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_ */
