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

#ifndef SRC_SQL_ENGINE_BASIC_OB_FILE_PARQUET_READER_H_
#define SRC_SQL_ENGINE_BASIC_OB_FILE_PARQUET_READER_H_

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

/**
 * A reader of the parquet format which is good enough for the file table
 * functions. It supports the flat schemas, the plain and the dictionary
 * encodings and the uncompressed and the snappy compressed pages. The rows
 * are produced one row group at a time so that the whole file is never
 * materialized in the memory.
 */
class ObFileParquetReader
{
public:
  ObFileParquetReader();
  ~ObFileParquetReader();

  int init(const common::ObString &path, common::ObIAllocator &allocator);
  int close();

  const common::ObSEArray<ObFileColumnSchema, 16> &get_columns() const { return columns_; }
  /* the number of the rows of the whole file */
  int64_t get_row_count() const { return num_rows_; }

  /**
   * Read the next row of the file. Returns OB_ITER_END when the file is
   * exhausted, otherwise the number of the produced columns is returned in
   * row_count.
   */
  int next_row(ObFileValue *values, int64_t capacity, int64_t &row_count);
  /* restart the scan */
  int reset();

private:
  struct ObParquetColumnMeta
  {
    ObParquetColumnMeta()
        : type_(-1), type_length_(0), codec_(0), num_values_(0),
          data_page_offset_(0), dictionary_page_offset_(OB_INVALID_ID) {}
    int32_t type_;
    int32_t type_length_;
    int32_t codec_;
    int64_t num_values_;
    int64_t data_page_offset_;
    int64_t dictionary_page_offset_;
  };

  struct ObParquetRowGroupMeta
  {
    ObParquetRowGroupMeta() : num_rows_(0), columns_() {}
    int64_t num_rows_;
    common::ObSEArray<ObParquetColumnMeta, 16> columns_;
  };

  struct ObParquetColumnValues
  {
    ObParquetColumnValues() : type_(OB_FILE_TYPE_VARCHAR), nullable_(false), values_() {}
    ObFileInferredType type_;
    bool nullable_;
    common::ObSEArray<ObFileValue, 16> values_;
  };

  int read_at(int64_t offset, int64_t size, char *buf);
  int load_file_metadata();
  int load_row_group(int64_t row_group_index);
  int decode_column_chunk(const ObParquetColumnMeta &meta,
                          const ObParquetColumnValues &column,
                          common::ObSEArray<ObFileValue, 16> &values);
  int decompress_page(int32_t codec,
                      const char *src,
                      int64_t src_len,
                      int64_t dst_len,
                      char *dst);
  int decode_page_values(const ObParquetColumnMeta &meta,
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
                         common::ObSEArray<ObFileValue, 16> &values);

  int fd_;
  int64_t file_size_;
  common::ObIAllocator *allocator_;
  common::ObSEArray<ObFileColumnSchema, 16> columns_;
  common::ObSEArray<ObParquetRowGroupMeta, 4> row_groups_;
  common::ObSEArray<ObParquetColumnValues, 16> column_values_;
  common::ObSEArray<int32_t, 16> schema_type_lengths_;
  int64_t num_rows_;
  int64_t row_group_index_;
  int64_t row_index_;
  bool loaded_;
};

/**
 * The iterator which scans a parquet file.
 */
class ObFileParquetIterator : public ObFileScanIterator
{
public:
  ObFileParquetIterator() : reader_() {}
  int init(const ObFileTableDescriptor &desc, common::ObIAllocator &allocator);
  int next_row(ObFileValue *values, int64_t capacity, int64_t &row_count) override;
  int reset() override;

private:
  ObFileParquetReader reader_;
};

} // end namespace sql
} // end namespace oceanbase

#endif /* SRC_SQL_ENGINE_BASIC_OB_FILE_PARQUET_READER_H_ */