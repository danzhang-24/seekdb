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

#include <gtest/gtest.h>

#include "lib/encode/ob_base64_encode.h"
#include "sql/engine/basic/ob_file_parquet_reader.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

bool str_eq(const ObString &str, const char *expected)
{
  const int32_t len = static_cast<int32_t>(::strlen(expected));
  return str.length() == len && 0 == ::memcmp(str.ptr(), expected, len);
}

const char *CSV_CONTENT =
    "id,name,score,active,day,created,note,id\n"
    "1,Alice,98.5,true,2026-08-01,2026-08-01 10:11:12,\"hello,\nworld\",101\n"
    "2,Bob,87,false,,,plain,102\n"
    "3,Chen,90.25,true,2026-08-03,2026-08-03T12:13:14,\"say \"\"hi\"\"\",103\n";

const char *JSONL_CONTENT =
    "{\"id\":1,\"name\":\"Alice\",\"score\":98.5,\"active\":true}\n"
    "{\"id\":2,\"name\":\"Bob\",\"active\":false,\"day\":\"2026-08-02\"}\n"
    "{\"id\":3,\"name\":\"Chen\",\"score\":90,\"active\":true,\"note\":\"ok\"}\n";

/* the parquet fixture of the file sql test suite */
const char *PARQUET_BASE64 =
    "UEFSMRUGFTwVQFwVDhUAFQ4VABUEFQARHDYAKAQGAAAAGAQAAAAAAAAAA38cbAAAAAABAAAAAgAAAAMAAAAEAAAABQAAAAYAAAAm"
    "lAEcFQIZJQYAGRgCYzEVAhYOFogBFowBJgg8NgAoBAYAAAAYBAAAAAAAAAAVBBksSARyb290FQIAFQIlAhgCYzEAFg4ZHBkcJpQB"
    "HBUCGSUGABkYAmMxFQIWDhaIARaMASYIPDYAKAQGAAAAGAQAAAAAAAAAFowBFg4AGRwYDEFSUk9XOnNjaGVtYRi8AS8vLy8vNFFB"
    "QUFBUUFBQUFBQUFLQUE0QURBQUxBQVFBQ2dBQUFCUUFBQUFBQUFBQkJBQUtBQXdBQUFBSUFBUUFDZ0FBQUFnQUFBQUlBQUFBQUFB"
    "QUFBRUFBQUFVQUFBQUVBQVVBQkFBRGdBUEFBUUFBQUFJQUJBQUFBQVlBQUFBSUFBQUFBQUFBUUljQUFBQUNBQU1BQVFBQ3dBSUFB"
    "QUFJQUFBQUFBQUFBRUFBQUFBQWdBQUFHTXhBQUE9ABgsQXJyb3cyIC0gTmF0aXZlIFJ1c3QgaW1wbGVtZW50YXRpb24gb2YgQXJy"
    "b3cATgEAAFBBUjE=";

/* a parquet file whose page is compressed by snappy */
const char *PARQUET_SNAPPY_BASE64 =
    "UEFSMRUAFXAVYCwVDhUAFQYVBgAAOAAADQEAAQEFDAAAAAIBBQwAAAADAQUMAAAABAEFDAAAAAUBBQwAAAAGAQUIAAAAFQIZLEgG"
    "c2NoZW1hFQIAFQQlABgCYzEAFg4ZHBkcJggcFQQZFQAZGAJjMRUCFg4WcBZgJggAABZwFg4AKBRzZWVrZGIgZmlsZSBzcWwgdGVz"
    "dABTAAAAUEFSMQ==";

/* a parquet file whose page is compressed by snappy and whose values are
 * encoded with the dictionary */
const char *PARQUET_SNAPPY_DICT_BASE64 =
    "UEFSMRUEFTAVKkwVBhUAFQYVBgAAGAQKAAkBABQBBQwAAAAeAQUIAAAAFQAVHhUULBUOFQQVBhUGAAAPFAICAAIBAhUGFQIZLEgG"
    "c2NoZW1hFQIAFQQlABgCYzIAFg4ZHBkcJlQcFQQZJQAEGRgCYzIVAhYOFh4WFCZUJggAABYeFg4AKBRzZWVrZGIgZmlsZSBzcWwg"
    "dGVzdABWAAAAUEFSMQ==";

class ObFileScanUtilsTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  std::string dir_;

  void SetUp() override
  {
    dir_ = "/tmp/ob_file_scan_test_" + std::to_string(::getpid());
    ::mkdir(dir_.c_str(), 0755);
  }

  void TearDown() override
  {
    const std::string cmd = "rm -rf " + dir_;
    if (0 != ::system(cmd.c_str())) {
      // the directory is left behind when it cannot be removed
    }
  }

  std::string write_file(const std::string &name, const std::string &content)
  {
    const std::string path = dir_ + "/" + name;
    FILE *file = ::fopen(path.c_str(), "wb");
    if (NULL == file) {
      return "";
    }
    ::fwrite(content.data(), 1, content.size(), file);
    ::fclose(file);
    return path;
  }

  std::string write_parquet(const std::string &name)
  {
    return write_base64_file(name, PARQUET_BASE64);
  }

  std::string write_base64_file(const std::string &name, const char *base64)
  {
    const std::string path = dir_ + "/" + name;
    uint8_t decoded[4096];
    int64_t pos = 0;
    const int64_t len = static_cast<int64_t>(::strlen(base64));
    if (OB_SUCCESS != ObBase64Encoder::decode(base64, len, decoded, sizeof(decoded), pos)) {
      return "";
    }
    FILE *file = ::fopen(path.c_str(), "wb");
    if (NULL == file) {
      return "";
    }
    ::fwrite(decoded, 1, pos, file);
    ::fclose(file);
    return path;
  }
};

TEST_F(ObFileScanUtilsTest, infer_scalar_type)
{
  ASSERT_EQ(OB_FILE_TYPE_BOOLEAN, ObFileTypeUtil::infer_scalar_type("true", 4));
  ASSERT_EQ(OB_FILE_TYPE_BOOLEAN, ObFileTypeUtil::infer_scalar_type("false", 5));
  ASSERT_EQ(OB_FILE_TYPE_BIGINT, ObFileTypeUtil::infer_scalar_type("123", 3));
  ASSERT_EQ(OB_FILE_TYPE_BIGINT, ObFileTypeUtil::infer_scalar_type("-7", 2));
  ASSERT_EQ(OB_FILE_TYPE_DOUBLE, ObFileTypeUtil::infer_scalar_type("98.5", 4));
  ASSERT_EQ(OB_FILE_TYPE_DATE, ObFileTypeUtil::infer_scalar_type("2026-08-01", 10));
  ASSERT_EQ(OB_FILE_TYPE_DATETIME, ObFileTypeUtil::infer_scalar_type("2026-08-01 10:11:12", 19));
  ASSERT_EQ(OB_FILE_TYPE_DATETIME, ObFileTypeUtil::infer_scalar_type("2026-08-01T10:11:12", 19));
  ASSERT_EQ(OB_FILE_TYPE_VARCHAR, ObFileTypeUtil::infer_scalar_type("Alice", 5));
  ASSERT_EQ(OB_FILE_TYPE_NULL, ObFileTypeUtil::infer_scalar_type("", 0));
}

TEST_F(ObFileScanUtilsTest, infer_scalar_type_merges_to_varchar)
{
  ASSERT_EQ(OB_FILE_TYPE_VARCHAR, ObFileTypeUtil::merge_type(OB_FILE_TYPE_BIGINT, OB_FILE_TYPE_VARCHAR));
  ASSERT_EQ(OB_FILE_TYPE_DOUBLE, ObFileTypeUtil::merge_type(OB_FILE_TYPE_BIGINT, OB_FILE_TYPE_DOUBLE));
  ASSERT_EQ(OB_FILE_TYPE_BIGINT, ObFileTypeUtil::merge_type(OB_FILE_TYPE_BIGINT, OB_FILE_TYPE_BIGINT));
  ASSERT_EQ(OB_FILE_TYPE_BOOLEAN, ObFileTypeUtil::merge_type(OB_FILE_TYPE_NULL, OB_FILE_TYPE_BOOLEAN));
}

TEST_F(ObFileScanUtilsTest, csv_schema_and_rows)
{
  const std::string path = write_file("basic.csv", CSV_CONTENT);
  ASSERT_FALSE(path.empty());

  ObFileTableDescriptor desc;
  desc.format_ = OB_FILE_FORMAT_CSV;
  desc.path_.assign_ptr(path.c_str(), path.size());
  desc.options_.has_header_ = true;

  ObSEArray<ObFileColumnSchema, 16> columns;
  int64_t file_size = 0;
  int64_t file_mtime = 0;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::infer_csv_schema(allocator_, desc, columns, file_size, file_mtime));
  ASSERT_EQ(8, columns.count());
  ASSERT_STREQ("BIGINT", ObFileTypeUtil::type_name(columns.at(0).type_));
  ASSERT_STREQ("VARCHAR", ObFileTypeUtil::type_name(columns.at(1).type_));
  ASSERT_STREQ("DOUBLE", ObFileTypeUtil::type_name(columns.at(2).type_));
  ASSERT_STREQ("BOOLEAN", ObFileTypeUtil::type_name(columns.at(3).type_));
  ASSERT_STREQ("DATE", ObFileTypeUtil::type_name(columns.at(4).type_));
  ASSERT_STREQ("DATETIME", ObFileTypeUtil::type_name(columns.at(5).type_));
  ASSERT_STREQ("VARCHAR", ObFileTypeUtil::type_name(columns.at(6).type_));
  ASSERT_STREQ("BIGINT", ObFileTypeUtil::type_name(columns.at(7).type_));
  ASSERT_TRUE(columns.at(4).nullable_);
  ASSERT_TRUE(columns.at(5).nullable_);
  ASSERT_FALSE(columns.at(0).nullable_);

  /* the duplicate header is made unique */
  ObFileScanUtils::uniquify_column_names(allocator_, columns);
  ASSERT_EQ(8, columns.count());
  ASSERT_TRUE(str_eq(columns.at(0).name_, "id"));
  ASSERT_TRUE(str_eq(columns.at(7).name_, "id__2"));

  desc.columns_.assign(columns);
  ObFileScanIterator *iterator = NULL;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::create_iterator(allocator_, desc, iterator));
  ASSERT_TRUE(NULL != iterator);
  ObFileValue values[16];
  int64_t row_count = 0;
  ASSERT_EQ(OB_SUCCESS, iterator->next_row(values, 16, row_count));
  ASSERT_EQ(8, row_count);
  ASSERT_EQ(1, values[0].int_val_);
  ASSERT_TRUE(str_eq(values[1].str_val_, "Alice"));
  ASSERT_DOUBLE_EQ(98.5, values[2].double_val_);
  ASSERT_EQ(1, values[3].int_val_);
  ASSERT_FALSE(values[4].is_null_);
  ASSERT_FALSE(values[5].is_null_);
  /* the quoted value holds the new line */
  ASSERT_EQ(12, values[6].str_val_.length());
  ASSERT_EQ(101, values[7].int_val_);

  ASSERT_EQ(OB_SUCCESS, iterator->next_row(values, 16, row_count));
  ASSERT_EQ(8, row_count);
  ASSERT_EQ(2, values[0].int_val_);
  ASSERT_DOUBLE_EQ(87.0, values[2].double_val_);
  ASSERT_TRUE(values[4].is_null_);
  ASSERT_TRUE(values[5].is_null_);

  ASSERT_EQ(OB_SUCCESS, iterator->next_row(values, 16, row_count));
  ASSERT_EQ(8, row_count);
  ASSERT_EQ(3, values[0].int_val_);
  ASSERT_DOUBLE_EQ(90.25, values[2].double_val_);
  ASSERT_TRUE(str_eq(values[6].str_val_, "say \"hi\""));

  /* the file is exhausted */
  ASSERT_EQ(OB_ITER_END, iterator->next_row(values, 16, row_count));
  ASSERT_EQ(0, row_count);
}

TEST_F(ObFileScanUtilsTest, uniquify_detects_the_names_which_are_not_terminated)
{
  /* the names of the columns are copied without the terminator, so they must
   * be compared by the length */
  char names[8] = {'i', 'd', 'x', 'x', 'i', 'd', 'y', 'y'};
  ObSEArray<ObFileColumnSchema, 16> columns;
  for (int64_t i = 0; i < 2; ++i) {
    ObFileColumnSchema col;
    col.type_ = OB_FILE_TYPE_BIGINT;
    col.name_.assign_ptr(names + i * 4, 2);
    ASSERT_EQ(OB_SUCCESS, columns.push_back(col));
  }

  ObFileScanUtils::uniquify_column_names(allocator_, columns);
  ASSERT_EQ(2, columns.count());
  ASSERT_TRUE(str_eq(columns.at(0).name_, "id"));
  ASSERT_TRUE(str_eq(columns.at(1).name_, "id__2"));
}

TEST_F(ObFileScanUtilsTest, jsonl_schema_and_rows)
{
  const std::string path = write_file("basic.jsonl", JSONL_CONTENT);
  ASSERT_FALSE(path.empty());

  ObFileTableDescriptor desc;
  desc.format_ = OB_FILE_FORMAT_JSONL;
  desc.path_.assign_ptr(path.c_str(), path.size());

  ObSEArray<ObFileColumnSchema, 16> columns;
  int64_t file_size = 0;
  int64_t file_mtime = 0;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::infer_jsonl_schema(allocator_, desc, columns, file_size, file_mtime));
  ASSERT_EQ(6, columns.count());
  ASSERT_TRUE(str_eq(columns.at(0).name_, "id"));
  ASSERT_TRUE(str_eq(columns.at(1).name_, "name"));
  ASSERT_TRUE(str_eq(columns.at(2).name_, "score"));
  ASSERT_TRUE(str_eq(columns.at(3).name_, "active"));
  ASSERT_TRUE(str_eq(columns.at(4).name_, "day"));
  ASSERT_TRUE(str_eq(columns.at(5).name_, "note"));
  ASSERT_STREQ("BIGINT", ObFileTypeUtil::type_name(columns.at(0).type_));
  ASSERT_STREQ("DOUBLE", ObFileTypeUtil::type_name(columns.at(2).type_));
  ASSERT_STREQ("BOOLEAN", ObFileTypeUtil::type_name(columns.at(3).type_));

  desc.columns_.assign(columns);
  ObFileScanIterator *iterator = NULL;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::create_iterator(allocator_, desc, iterator));
  ASSERT_TRUE(NULL != iterator);
  ObFileValue values[16];
  int64_t row_count = 0;
  ASSERT_EQ(OB_SUCCESS, iterator->next_row(values, 16, row_count));
  ASSERT_EQ(6, row_count);
  ASSERT_EQ(1, values[0].int_val_);
  ASSERT_TRUE(str_eq(values[1].str_val_, "Alice"));
  ASSERT_DOUBLE_EQ(98.5, values[2].double_val_);

  ASSERT_EQ(OB_SUCCESS, iterator->next_row(values, 16, row_count));
  ASSERT_EQ(6, row_count);
  ASSERT_EQ(2, values[0].int_val_);
  ASSERT_TRUE(values[2].is_null_);
  ASSERT_FALSE(values[4].is_null_);

  ASSERT_EQ(OB_SUCCESS, iterator->next_row(values, 16, row_count));
  ASSERT_EQ(6, row_count);
  ASSERT_EQ(3, values[0].int_val_);
  ASSERT_DOUBLE_EQ(90.0, values[2].double_val_);
  ASSERT_TRUE(str_eq(values[5].str_val_, "ok"));

  ASSERT_EQ(OB_ITER_END, iterator->next_row(values, 16, row_count));
  ASSERT_EQ(0, row_count);
}

TEST_F(ObFileScanUtilsTest, parquet_schema_and_rows)
{
  const std::string path = write_parquet("simple.parquet");
  ASSERT_FALSE(path.empty());

  ObFileTableDescriptor desc;
  desc.format_ = OB_FILE_FORMAT_PARQUET;
  desc.path_.assign_ptr(path.c_str(), path.size());

  ObSEArray<ObFileColumnSchema, 16> columns;
  int64_t file_size = 0;
  int64_t file_mtime = 0;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::infer_parquet_schema(allocator_, desc, columns, file_size, file_mtime));
  ASSERT_EQ(1, columns.count());
  ASSERT_TRUE(str_eq(columns.at(0).name_, "c1"));
  ASSERT_STREQ("BIGINT", ObFileTypeUtil::type_name(columns.at(0).type_));

  desc.columns_.assign(columns);
  ObFileScanIterator *iterator = NULL;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::create_iterator(allocator_, desc, iterator));
  ASSERT_TRUE(NULL != iterator);
  ObFileValue values[16];
  int64_t row_count = 0;
  int64_t sum = 0;
  int64_t count = 0;
  int64_t min_value = INT64_MAX;
  int64_t max_value = INT64_MIN;
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret)) {
    ret = iterator->next_row(values, 16, row_count);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    }
    ASSERT_EQ(OB_SUCCESS, ret);
    if (0 == row_count) {
      break;
    }
    ASSERT_EQ(1, row_count);
    ASSERT_FALSE(values[0].is_null_);
    sum += values[0].int_val_;
    min_value = std::min(min_value, values[0].int_val_);
    max_value = std::max(max_value, values[0].int_val_);
    ++count;
  }
  ASSERT_EQ(7, count);
  ASSERT_EQ(21, sum);
  ASSERT_EQ(0, min_value);
  ASSERT_EQ(6, max_value);
}

TEST_F(ObFileScanUtilsTest, glob_lists_the_files)
{
  write_file("a.csv", "id\n1\n");
  write_file("b.jsonl", "{}\n");

  ObSEArray<ObString, 16> files;
  const std::string pattern = dir_ + "/*";
  ASSERT_EQ(OB_SUCCESS,
            ObFileScanUtils::expand_glob(allocator_, ObString(pattern.size(), pattern.c_str()), files));
  ASSERT_EQ(2, files.count());

  ObFileTableDescriptor desc;
  desc.format_ = OB_FILE_FORMAT_GLOB;
  desc.path_.assign_ptr(pattern.c_str(), pattern.size());
  ObSEArray<ObFileColumnSchema, 16> columns;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::infer_glob_schema(allocator_, desc, columns));
  ASSERT_EQ(1, columns.count());
  ASSERT_TRUE(str_eq(columns.at(0).name_, "file"));

  desc.columns_.assign(columns);
  ObFileScanIterator *iterator = NULL;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::create_iterator(allocator_, desc, iterator));
  ASSERT_TRUE(NULL != iterator);
  ObFileValue values[16];
  int64_t row_count = 0;
  int64_t count = 0;
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret)) {
    ret = iterator->next_row(values, 16, row_count);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    }
    ASSERT_EQ(OB_SUCCESS, ret);
    if (0 == row_count) {
      break;
    }
    ASSERT_EQ(1, row_count);
    ASSERT_TRUE(values[0].str_val_.length() > 0);
    ++count;
  }
  ASSERT_EQ(2, count);
}

TEST_F(ObFileScanUtilsTest, descriptor_round_trip)
{
  ObFileTableDescriptor desc;
  desc.format_ = OB_FILE_FORMAT_CSV;
  desc.path_.assign_ptr("/tmp/a.csv", 10);
  desc.options_.has_header_ = false;
  desc.options_.delimiter_ = ';';
  desc.options_.sample_size_ = -1;
  desc.file_size_ = 1234;
  desc.file_mtime_ = 5678;
  ObFileColumnSchema column;
  column.name_.assign_ptr("id", 2);
  column.type_ = OB_FILE_TYPE_BIGINT;
  column.nullable_ = true;
  ASSERT_EQ(OB_SUCCESS, desc.columns_.push_back(column));

  ObString encoded;
  ASSERT_EQ(OB_SUCCESS, desc.encode(allocator_, encoded));
  ASSERT_TRUE(ObFileTableDescriptor::is_file_descriptor(encoded));

  ObFileTableDescriptor decoded;
  ASSERT_EQ(OB_SUCCESS, ObFileTableDescriptor::decode(allocator_, encoded, decoded));
  ASSERT_EQ(OB_FILE_FORMAT_CSV, decoded.format_);
  ASSERT_TRUE(str_eq(decoded.path_, "/tmp/a.csv"));
  ASSERT_FALSE(decoded.options_.has_header_);
  ASSERT_EQ(';', decoded.options_.delimiter_);
  ASSERT_EQ(1234, decoded.file_size_);
  ASSERT_EQ(5678, decoded.file_mtime_);
  ASSERT_EQ(1, decoded.columns_.count());
  ASSERT_TRUE(str_eq(decoded.columns_.at(0).name_, "id"));
  ASSERT_EQ(OB_FILE_TYPE_BIGINT, decoded.columns_.at(0).type_);
  ASSERT_TRUE(decoded.columns_.at(0).nullable_);
}

TEST_F(ObFileScanUtilsTest, parquet_snappy_schema_and_rows)
{
  const std::string path = write_base64_file("snappy.parquet", PARQUET_SNAPPY_BASE64);
  ASSERT_FALSE(path.empty());

  ObFileTableDescriptor desc;
  desc.format_ = OB_FILE_FORMAT_PARQUET;
  desc.path_.assign_ptr(path.c_str(), path.size());

  ObSEArray<ObFileColumnSchema, 16> columns;
  int64_t file_size = 0;
  int64_t file_mtime = 0;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::infer_parquet_schema(allocator_, desc, columns, file_size, file_mtime));
  ASSERT_EQ(1, columns.count());
  ASSERT_TRUE(str_eq(columns.at(0).name_, "c1"));
  ASSERT_STREQ("BIGINT", ObFileTypeUtil::type_name(columns.at(0).type_));

  desc.columns_.assign(columns);
  ObFileScanIterator *iterator = NULL;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::create_iterator(allocator_, desc, iterator));
  ASSERT_TRUE(NULL != iterator);
  ObFileValue values[16];
  int64_t row_count = 0;
  int64_t sum = 0;
  int64_t count = 0;
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret)) {
    ret = iterator->next_row(values, 16, row_count);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    }
    ASSERT_EQ(OB_SUCCESS, ret);
    if (0 == row_count) {
      break;
    }
    ASSERT_EQ(1, row_count);
    ASSERT_FALSE(values[0].is_null_);
    /* the values of the page are decompressed by snappy */
    ASSERT_EQ(count, values[0].int_val_);
    sum += values[0].int_val_;
    ++count;
  }
  ASSERT_EQ(7, count);
  ASSERT_EQ(21, sum);
}

TEST_F(ObFileScanUtilsTest, parquet_snappy_dictionary_schema_and_rows)
{
  const std::string path = write_base64_file("snappy_dict.parquet", PARQUET_SNAPPY_DICT_BASE64);
  ASSERT_FALSE(path.empty());

  ObFileTableDescriptor desc;
  desc.format_ = OB_FILE_FORMAT_PARQUET;
  desc.path_.assign_ptr(path.c_str(), path.size());

  ObSEArray<ObFileColumnSchema, 16> columns;
  int64_t file_size = 0;
  int64_t file_mtime = 0;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::infer_parquet_schema(allocator_, desc, columns, file_size, file_mtime));
  ASSERT_EQ(1, columns.count());
  ASSERT_TRUE(str_eq(columns.at(0).name_, "c2"));
  ASSERT_STREQ("BIGINT", ObFileTypeUtil::type_name(columns.at(0).type_));

  desc.columns_.assign(columns);
  ObFileScanIterator *iterator = NULL;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::create_iterator(allocator_, desc, iterator));
  ASSERT_TRUE(NULL != iterator);
  ObFileValue values[16];
  int64_t row_count = 0;
  int64_t count = 0;
  const int64_t expected[7] = {10, 20, 30, 10, 20, 30, 10};
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret)) {
    ret = iterator->next_row(values, 16, row_count);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    }
    ASSERT_EQ(OB_SUCCESS, ret);
    if (0 == row_count) {
      break;
    }
    ASSERT_EQ(1, row_count);
    ASSERT_FALSE(values[0].is_null_);
    /* the values are the indices of the dictionary */
    ASSERT_EQ(expected[count], values[0].int_val_);
    ++count;
  }
  ASSERT_EQ(7, count);
}

TEST_F(ObFileScanUtilsTest, file_change_is_detected)
{
  const std::string path = write_file("change.csv", "id,name\n1,Alice\n");
  ASSERT_FALSE(path.empty());
  const ObString path_str(path.size(), path.c_str());

  int64_t file_size = 0;
  int64_t file_mtime = 0;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::stat_file(path_str, file_size, file_mtime));
  ASSERT_TRUE(file_size > 0);

  ObFileTableDescriptor desc;
  desc.format_ = OB_FILE_FORMAT_CSV;
  desc.path_ = path_str;
  desc.options_.has_header_ = true;
  ObSEArray<ObFileColumnSchema, 16> columns;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::infer_csv_schema(allocator_, desc, columns, file_size, file_mtime));
  ASSERT_EQ(2, columns.count());
  desc.columns_.assign(columns);
  desc.file_size_ = file_size;
  desc.file_mtime_ = file_mtime;

  /* the file is not changed */
  int64_t new_size = 0;
  int64_t new_mtime = 0;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::stat_file(path_str, new_size, new_mtime));
  ASSERT_EQ(desc.file_size_, new_size);
  ASSERT_EQ(desc.file_mtime_, new_mtime);

  /* the file is changed, the schema of the file table follows its content */
  write_file("change.csv", "id,name,score\n1,Alice,9\n2,Bob,8\n");
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::stat_file(path_str, new_size, new_mtime));
  ASSERT_TRUE(new_size != desc.file_size_);

  ObSEArray<ObFileColumnSchema, 16> new_columns;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::infer_csv_schema(allocator_, desc, new_columns, new_size, new_mtime));
  ASSERT_EQ(3, new_columns.count());
  ASSERT_STREQ("BIGINT", ObFileTypeUtil::type_name(new_columns.at(2).type_));

  /* the rows of the changed file are read from the new content */
  desc.columns_.assign(new_columns);
  ObFileScanIterator *iterator = NULL;
  ASSERT_EQ(OB_SUCCESS, ObFileScanUtils::create_iterator(allocator_, desc, iterator));
  ASSERT_TRUE(NULL != iterator);
  ObFileValue values[16];
  int64_t row_count = 0;
  int64_t count = 0;
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret)) {
    ret = iterator->next_row(values, 16, row_count);
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      break;
    }
    ASSERT_EQ(OB_SUCCESS, ret);
    if (0 == row_count) {
      break;
    }
    ASSERT_EQ(3, row_count);
    ++count;
  }
  ASSERT_EQ(2, count);
}

} // namespace
