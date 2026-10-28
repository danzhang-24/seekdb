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

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

#include <string.h>
#include <vector>

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

bool node_str_eq(const ParseNode *node, const char *expected)
{
  if (OB_ISNULL(node) || OB_ISNULL(node->str_value_)) {
    return false;
  }
  const int32_t len = static_cast<int32_t>(::strlen(expected));
  return node->str_len_ == len && 0 == ::memcmp(node->str_value_, expected, len);
}

class ObFileSqlParserTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
  ParseResult parse_result_;

  int parse(const char *sql)
  {
    ObParser parser(allocator_, static_cast<ObSQLMode>(0));
    return parser.parse(ObString(static_cast<int32_t>(::strlen(sql)), sql), parse_result_, STD_MODE);
  }

  /* the statement of the parse result, the result tree is a statement list */
  const ParseNode *stmt() const
  {
    const ParseNode *tree = parse_result_.result_tree_;
    if (OB_ISNULL(tree)) {
      return NULL;
    }
    if (T_STMT_LIST == tree->type_) {
      return tree->num_child_ > 0 ? tree->children_[0] : NULL;
    }
    return tree;
  }

  /* the file table expression of the first item of the from list */
  static const ParseNode *file_table_expr(const ParseNode *select_node)
  {
    if (OB_ISNULL(select_node) || T_SELECT != select_node->type_) {
      return NULL;
    }
    const ParseNode *from_list = select_node->children_[PARSE_SELECT_FROM];
    if (OB_ISNULL(from_list) || T_FROM_LIST != from_list->type_ || from_list->num_child_ < 1) {
      return NULL;
    }
    const ParseNode *factor = from_list->children_[0];
    if (OB_ISNULL(factor) || T_FILE_TABLE_EXPRESSION != factor->type_) {
      return NULL;
    }
    return factor;
  }

  /* the path literal of the file table expression */
  static const ParseNode *file_table_path(const ParseNode *file_expr)
  {
    if (OB_ISNULL(file_expr) || file_expr->num_child_ < 2) {
      return NULL;
    }
    const ParseNode *args = file_expr->children_[1];
    if (OB_ISNULL(args) || T_FILE_TABLE_ARGS != args->type_ || args->num_child_ < 1) {
      return NULL;
    }
    return args->children_[0];
  }

  /* the option list of the file table expression */
  static const ParseNode *file_table_options(const ParseNode *file_expr)
  {
    if (OB_ISNULL(file_expr) || file_expr->num_child_ < 2) {
      return NULL;
    }
    const ParseNode *args = file_expr->children_[1];
    if (OB_ISNULL(args) || T_FILE_TABLE_ARGS != args->type_ || args->num_child_ < 2) {
      return NULL;
    }
    return args->children_[1];
  }

  static const ParseNode *option_at(const ParseNode *option_list, int64_t index)
  {
    if (OB_ISNULL(option_list) || T_EXPR_LIST != option_list->type_
        || index >= option_list->num_child_) {
      return NULL;
    }
    return option_list->children_[index];
  }

  /* collect all the file table expressions of the tree */
  static void collect_file_exprs(const ParseNode *node, std::vector<const ParseNode *> &result)
  {
    if (OB_ISNULL(node)) {
      return;
    }
    if (T_FILE_TABLE_EXPRESSION == node->type_) {
      result.push_back(node);
      return;
    }
    for (int64_t i = 0; i < node->num_child_; ++i) {
      collect_file_exprs(node->children_[i], result);
    }
  }
};

TEST_F(ObFileSqlParserTest, parse_file_table_functions)
{
  const char *sqls[] = {
    "select * from read_csv('/tmp/file_sql/a.csv')",
    "select * from read_json('/tmp/file_sql/a.jsonl')",
    "select * from read_ndjson('/tmp/file_sql/a.jsonl')",
    "select * from read_parquet('/tmp/file_sql/a.parquet')",
    "select * from glob('/tmp/file_sql/*')",
  };
  const char *names[] = {"read_csv", "read_json", "read_ndjson", "read_parquet", "glob"};
  const char *paths[] = {
    "/tmp/file_sql/a.csv", "/tmp/file_sql/a.jsonl", "/tmp/file_sql/a.jsonl",
    "/tmp/file_sql/a.parquet", "/tmp/file_sql/*",
  };

  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    ASSERT_EQ(OB_SUCCESS, parse(sqls[i])) << sqls[i];
    const ParseNode *file_expr = file_table_expr(stmt());
    ASSERT_TRUE(NULL != file_expr) << sqls[i];
    ASSERT_TRUE(NULL != file_expr->children_[0]);
    ASSERT_TRUE(node_str_eq(file_expr->children_[0], names[i])) << sqls[i];

    const ParseNode *path = file_table_path(file_expr);
    ASSERT_TRUE(NULL != path) << sqls[i];
    ASSERT_EQ(T_VARCHAR, path->type_) << sqls[i];
    ASSERT_TRUE(node_str_eq(path, paths[i])) << sqls[i];

    /* the path and the options are literals, they are not parameterized */
    ASSERT_TRUE(NULL == file_table_options(file_expr)) << sqls[i];
  }
}

TEST_F(ObFileSqlParserTest, parse_file_table_alias)
{
  ASSERT_EQ(OB_SUCCESS, parse("select f.id from read_csv('/tmp/file_sql/a.csv') f"));
  const ParseNode *file_expr = file_table_expr(stmt());
  ASSERT_TRUE(NULL != file_expr);
  ASSERT_EQ(4, file_expr->num_child_);
  ASSERT_TRUE(NULL != file_expr->children_[3]);
  ASSERT_TRUE(node_str_eq(file_expr->children_[3], "f"));

  ASSERT_EQ(OB_SUCCESS, parse("select f.id from read_csv('/tmp/file_sql/a.csv') AS f"));
  file_expr = file_table_expr(stmt());
  ASSERT_TRUE(NULL != file_expr);
  ASSERT_EQ(4, file_expr->num_child_);
  ASSERT_TRUE(NULL != file_expr->children_[3]);
  ASSERT_TRUE(node_str_eq(file_expr->children_[3], "f"));

  /* the alias is optional */
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/tmp/file_sql/a.csv')"));
  file_expr = file_table_expr(stmt());
  ASSERT_TRUE(NULL != file_expr);
  ASSERT_EQ(4, file_expr->num_child_);
  ASSERT_TRUE(NULL == file_expr->children_[3]);
}

TEST_F(ObFileSqlParserTest, parse_file_table_named_options)
{
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/tmp/file_sql/a.csv', header = true, "
                              "delimiter = ';', sample_size = -1, format = 'csv')"));
  const ParseNode *file_expr = file_table_expr(stmt());
  ASSERT_TRUE(NULL != file_expr);
  const ParseNode *options = file_table_options(file_expr);
  ASSERT_TRUE(NULL != options);
  ASSERT_EQ(T_EXPR_LIST, options->type_);
  ASSERT_EQ(4, options->num_child_);

  const ParseNode *option = option_at(options, 0);
  ASSERT_TRUE(NULL != option);
  ASSERT_EQ(T_FILE_TABLE_OPTION, option->type_);
  ASSERT_TRUE(node_str_eq(option->children_[0], "header"));
  ASSERT_EQ(T_BOOL, option->children_[1]->type_);
  ASSERT_EQ(1, option->children_[1]->value_);

  option = option_at(options, 1);
  ASSERT_TRUE(NULL != option);
  ASSERT_TRUE(node_str_eq(option->children_[0], "delimiter"));
  ASSERT_EQ(T_VARCHAR, option->children_[1]->type_);
  ASSERT_TRUE(node_str_eq(option->children_[1], ";"));

  option = option_at(options, 2);
  ASSERT_TRUE(NULL != option);
  ASSERT_TRUE(node_str_eq(option->children_[0], "sample_size"));
  ASSERT_EQ(T_INT, option->children_[1]->type_);
  ASSERT_EQ(-1, option->children_[1]->value_);

  option = option_at(options, 3);
  ASSERT_TRUE(NULL != option);
  ASSERT_TRUE(node_str_eq(option->children_[0], "format"));
  ASSERT_EQ(T_VARCHAR, option->children_[1]->type_);
  ASSERT_TRUE(node_str_eq(option->children_[1], "csv"));
}

TEST_F(ObFileSqlParserTest, parse_file_table_option_without_value)
{
  /* the option without a value is a switch, its value is one */
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/tmp/file_sql/a.csv', header)"));
  const ParseNode *options = file_table_options(file_table_expr(stmt()));
  ASSERT_TRUE(NULL != options);
  ASSERT_EQ(1, options->num_child_);
  const ParseNode *option = option_at(options, 0);
  ASSERT_TRUE(NULL != option);
  ASSERT_TRUE(node_str_eq(option->children_[0], "header"));
  ASSERT_EQ(T_INT, option->children_[1]->type_);
  ASSERT_EQ(1, option->children_[1]->value_);
}

TEST_F(ObFileSqlParserTest, parse_glob)
{
  ASSERT_EQ(OB_SUCCESS, parse("select file from glob('/tmp/file_sql/*.csv')"));
  const ParseNode *file_expr = file_table_expr(stmt());
  ASSERT_TRUE(NULL != file_expr);
  ASSERT_TRUE(node_str_eq(file_expr->children_[0], "glob"));
  const ParseNode *path = file_table_path(file_expr);
  ASSERT_TRUE(NULL != path);
  ASSERT_EQ(T_VARCHAR, path->type_);
  ASSERT_TRUE(node_str_eq(path, "/tmp/file_sql/*.csv"));
}

TEST_F(ObFileSqlParserTest, parse_two_file_tables_in_join)
{
  ASSERT_EQ(OB_SUCCESS, parse("select a.id, b.name from read_csv('/tmp/file_sql/a.csv') a "
                              "join read_json('/tmp/file_sql/b.jsonl') b on a.id = b.id"));
  std::vector<const ParseNode *> file_exprs;
  collect_file_exprs(stmt(), file_exprs);
  ASSERT_EQ(2, file_exprs.size());
  ASSERT_TRUE(node_str_eq(file_exprs.at(0)->children_[0], "read_csv"));
  ASSERT_TRUE(node_str_eq(file_exprs.at(1)->children_[0], "read_json"));
  ASSERT_TRUE(node_str_eq(file_exprs.at(0)->children_[3], "a"));
  ASSERT_TRUE(node_str_eq(file_exprs.at(1)->children_[3], "b"));
}

TEST_F(ObFileSqlParserTest, parse_two_file_tables_in_cross_join)
{
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/tmp/file_sql/a.csv') a, "
                              "read_parquet('/tmp/file_sql/b.parquet') b"));
  std::vector<const ParseNode *> file_exprs;
  collect_file_exprs(stmt(), file_exprs);
  ASSERT_EQ(2, file_exprs.size());
  ASSERT_TRUE(node_str_eq(file_exprs.at(0)->children_[0], "read_csv"));
  ASSERT_TRUE(node_str_eq(file_exprs.at(1)->children_[0], "read_parquet"));
}

TEST_F(ObFileSqlParserTest, parse_describe_select)
{
  ASSERT_EQ(OB_SUCCESS, parse("describe select * from read_csv('/tmp/file_sql/a.csv')"));
  const ParseNode *desc = stmt();
  ASSERT_TRUE(NULL != desc);
  ASSERT_EQ(T_DESC_SELECT, desc->type_);
  ASSERT_EQ(1, desc->num_child_);
  ASSERT_EQ(T_SELECT, desc->children_[0]->type_);
  ASSERT_TRUE(NULL != file_table_expr(desc->children_[0]));

  ASSERT_EQ(OB_SUCCESS, parse("desc select * from read_parquet('/tmp/file_sql/a.parquet')"));
  desc = stmt();
  ASSERT_TRUE(NULL != desc);
  ASSERT_EQ(T_DESC_SELECT, desc->type_);
  ASSERT_TRUE(NULL != file_table_expr(desc->children_[0]));
}

TEST_F(ObFileSqlParserTest, parse_copy_select_to)
{
  ASSERT_EQ(OB_SUCCESS, parse("copy (select id, name from t1 order by id) to '/tmp/file_sql/out.csv' "
                              "(header, delimiter = ',', nullstr 'NULL')"));
  const ParseNode *select_node = stmt();
  ASSERT_TRUE(NULL != select_node);
  ASSERT_EQ(T_SELECT, select_node->type_);

  const ParseNode *into = select_node->children_[PARSE_SELECT_INTO];
  ASSERT_TRUE(NULL != into);
  ASSERT_EQ(T_COPY_INTO_OUTFILE, into->type_);
  ASSERT_EQ(2, into->num_child_);
  ASSERT_EQ(T_VARCHAR, into->children_[0]->type_);
  ASSERT_TRUE(node_str_eq(into->children_[0], "/tmp/file_sql/out.csv"));

  const ParseNode *options = into->children_[1];
  ASSERT_TRUE(NULL != options);
  ASSERT_EQ(T_EXPR_LIST, options->type_);
  ASSERT_EQ(3, options->num_child_);

  const ParseNode *option = option_at(options, 0);
  ASSERT_TRUE(NULL != option);
  ASSERT_EQ(T_FILE_TABLE_OPTION, option->type_);
  ASSERT_TRUE(node_str_eq(option->children_[0], "header"));
  ASSERT_EQ(T_INT, option->children_[1]->type_);
  ASSERT_EQ(1, option->children_[1]->value_);

  option = option_at(options, 1);
  ASSERT_TRUE(NULL != option);
  ASSERT_TRUE(node_str_eq(option->children_[0], "delimiter"));
  ASSERT_TRUE(node_str_eq(option->children_[1], ","));

  option = option_at(options, 2);
  ASSERT_TRUE(NULL != option);
  ASSERT_TRUE(node_str_eq(option->children_[0], "nullstr"));
  ASSERT_EQ(T_VARCHAR, option->children_[1]->type_);
  ASSERT_TRUE(node_str_eq(option->children_[1], "NULL"));
}

TEST_F(ObFileSqlParserTest, parse_copy_table_to)
{
  ASSERT_EQ(OB_SUCCESS, parse("copy t1 to '/tmp/file_sql/out.csv'"));
  const ParseNode *select_node = stmt();
  ASSERT_TRUE(NULL != select_node);
  ASSERT_EQ(T_SELECT, select_node->type_);

  const ParseNode *into = select_node->children_[PARSE_SELECT_INTO];
  ASSERT_TRUE(NULL != into);
  ASSERT_EQ(T_COPY_INTO_OUTFILE, into->type_);
  ASSERT_EQ(2, into->num_child_);
  ASSERT_EQ(T_VARCHAR, into->children_[0]->type_);
  ASSERT_TRUE(node_str_eq(into->children_[0], "/tmp/file_sql/out.csv"));
  /* the option list is optional */
  ASSERT_TRUE(NULL == into->children_[1]);
}

TEST_F(ObFileSqlParserTest, parse_copy_from_file_table)
{
  ASSERT_EQ(OB_SUCCESS, parse("copy (select * from read_csv('/tmp/file_sql/a.csv')) "
                              "to '/tmp/file_sql/out.csv' (header)"));
  const ParseNode *select_node = stmt();
  ASSERT_TRUE(NULL != select_node);
  ASSERT_TRUE(NULL != file_table_expr(select_node));
  const ParseNode *into = select_node->children_[PARSE_SELECT_INTO];
  ASSERT_TRUE(NULL != into);
  ASSERT_EQ(T_COPY_INTO_OUTFILE, into->type_);
}

TEST_F(ObFileSqlParserTest, parse_invalid_file_table_statements)
{
  /* the path must be a literal */
  ASSERT_NE(OB_SUCCESS, parse("select * from read_csv(path_column)"));
  /* the file table function requires the path */
  ASSERT_NE(OB_SUCCESS, parse("select * from read_csv()"));
  /* the copy statement requires the path */
  ASSERT_NE(OB_SUCCESS, parse("copy (select 1 from t1) to path_column"));
}

} // namespace
