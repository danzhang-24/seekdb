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
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace
{

// the json text is compared as std::string, so that gtest prints the real value on failure
std::string to_std_string(const ObString &str)
{
  return std::string(str.ptr(), str.length());
}

class ObExprAIPromptTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;
};

// build the args array of a prompt, every element is appended as it is
int build_args_array(ObIAllocator &allocator, ObJsonArray *&args_array)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, args_array))) {
  }
  return ret;
}

TEST_F(ObExprAIPromptTest, construct_prompt_object_with_string_arg)
{
  ObString template_str("{0}-ok");
  ObJsonArray *args_array = NULL;
  ObJsonString *arg_str = NULL;
  ObJsonObject *prompt_object = NULL;
  ObString arg("lob");

  ASSERT_EQ(OB_SUCCESS, build_args_array(allocator_, args_array));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, arg, arg_str));
  ASSERT_EQ(OB_SUCCESS, args_array->append(arg_str));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
                            allocator_, template_str, args_array, prompt_object));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));

  ObString text_str;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, prompt_object, text_str));
  EXPECT_EQ(std::string("{\"args\": [\"lob\"], \"template\": \"{0}-ok\"}"), to_std_string(text_str));
}

TEST_F(ObExprAIPromptTest, construct_prompt_object_with_json_args)
{
  // the ai_prompt function accepts json arguments, they are kept as json nodes
  ObString template_str("{0}/{0}/{1}");
  ObJsonArray *args_array = NULL;
  ObJsonObject *second_arg = NULL;
  ObJsonObject *prompt_object = NULL;

  ASSERT_EQ(OB_SUCCESS, build_args_array(allocator_, args_array));
  // build the array argument [1, true] explicitly
  ObJsonArray *number_array = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, number_array));
  ObJsonInt *int_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_int(allocator_, 1, int_node));
  ASSERT_EQ(OB_SUCCESS, number_array->append(int_node));
  ObJsonBoolean *bool_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_boolean(allocator_, true, bool_node));
  ASSERT_EQ(OB_SUCCESS, number_array->append(bool_node));

  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, second_arg));
  ObJsonInt *value_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_int(allocator_, 2, value_node));
  ASSERT_EQ(OB_SUCCESS, second_arg->add("k", value_node));

  ASSERT_EQ(OB_SUCCESS, args_array->append(number_array));
  ASSERT_EQ(OB_SUCCESS, args_array->append(second_arg));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
                            allocator_, template_str, args_array, prompt_object));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));

  ObString text_str;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, prompt_object, text_str));
  EXPECT_EQ(std::string("{\"args\": [[1, true], {\"k\": 2}], \"template\": \"{0}/{0}/{1}\"}"), to_std_string(text_str));
}

TEST_F(ObExprAIPromptTest, construct_prompt_object_rejects_empty_template)
{
  ObString template_str("");
  ObJsonArray *args_array = NULL;
  ObJsonObject *prompt_object = NULL;

  ASSERT_EQ(OB_SUCCESS, build_args_array(allocator_, args_array));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::construct_prompt_object(
                                    allocator_, template_str, args_array, prompt_object));
  ASSERT_TRUE(OB_ISNULL(prompt_object));
}

TEST_F(ObExprAIPromptTest, is_valid_prompt_object)
{
  ObString template_str("{0}");
  ObJsonArray *args_array = NULL;
  ObJsonObject *prompt_object = NULL;

  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(NULL));

  ASSERT_EQ(OB_SUCCESS, build_args_array(allocator_, args_array));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
                            allocator_, template_str, args_array, prompt_object));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));

  // an object without the args key is invalid
  ObJsonObject *broken_object = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, broken_object));
  ObJsonString *template_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, template_str, template_node));
  ASSERT_EQ(OB_SUCCESS, broken_object->add(ObAIFuncPromptObjectUtils::prompt_template_key, template_node));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(broken_object));
}

TEST_F(ObExprAIPromptTest, replace_all_str_args_in_template)
{
  ObString template_str("hello {0}, {1} and {0}!");
  ObJsonArray *args_array = NULL;
  ObJsonObject *prompt_object = NULL;
  ObString replaced_str;

  ASSERT_EQ(OB_SUCCESS, build_args_array(allocator_, args_array));
  ObString first_arg("world");
  ObJsonString *first_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, first_arg, first_node));
  ASSERT_EQ(OB_SUCCESS, args_array->append(first_node));
  ObString second_arg("oceanbase");
  ObJsonString *second_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, second_arg, second_node));
  ASSERT_EQ(OB_SUCCESS, args_array->append(second_node));

  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
                            allocator_, template_str, args_array, prompt_object));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced_str));
  ASSERT_EQ(ObString("hello world, oceanbase and world!"), replaced_str);
}

TEST_F(ObExprAIPromptTest, replace_all_str_args_in_template_with_json_arg)
{
  // a json argument is replaced by its json text
  ObString template_str("value: {0}");
  ObJsonArray *args_array = NULL;
  ObJsonObject *json_arg = NULL;
  ObJsonObject *prompt_object = NULL;
  ObString replaced_str;

  ASSERT_EQ(OB_SUCCESS, build_args_array(allocator_, args_array));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, json_arg));
  ObJsonInt *value_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_int(allocator_, 2, value_node));
  ASSERT_EQ(OB_SUCCESS, json_arg->add("k", value_node));
  ASSERT_EQ(OB_SUCCESS, args_array->append(json_arg));

  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
                            allocator_, template_str, args_array, prompt_object));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced_str));
  EXPECT_EQ(std::string("value: {\"k\": 2}"), to_std_string(replaced_str));
}

TEST_F(ObExprAIPromptTest, replace_all_str_args_in_template_rejects_unknown_index)
{
  ObString template_str("{3}");
  ObJsonArray *args_array = NULL;
  ObJsonObject *prompt_object = NULL;
  ObString replaced_str;

  ASSERT_EQ(OB_SUCCESS, build_args_array(allocator_, args_array));
  ObString arg("world");
  ObJsonString *arg_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, arg, arg_node));
  ASSERT_EQ(OB_SUCCESS, args_array->append(arg_node));

  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
                            allocator_, template_str, args_array, prompt_object));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                                    allocator_, prompt_object, replaced_str));
}

TEST_F(ObExprAIPromptTest, ob_is_json_array_valid_arg)
{
  ObJsonArray *array = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, array));
  ASSERT_TRUE(ObAIFuncJsonUtils::ob_is_json_array_valid_arg(array));

  ObJsonString *str_node = NULL;
  ObString str("hello");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, str, str_node));
  ASSERT_EQ(OB_SUCCESS, array->append(str_node));
  ASSERT_TRUE(ObAIFuncJsonUtils::ob_is_json_array_valid_arg(array));

  // a number argument is not allowed, it can not be sent to the ai service
  ObJsonInt *int_node = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_int(allocator_, 1, int_node));
  ASSERT_EQ(OB_SUCCESS, array->append(int_node));
  ASSERT_FALSE(ObAIFuncJsonUtils::ob_is_json_array_valid_arg(array));
}

} // namespace
