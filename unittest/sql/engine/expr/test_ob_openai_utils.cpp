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
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace
{

class ObOpenAIUtilsTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  int parse_json_object(const char *json_str, ObJsonObject *&obj)
  {
    ObString str(json_str);
    return ObAIFuncJsonUtils::get_json_object_form_str(allocator_, str, obj);
  }
};

TEST_F(ObOpenAIUtilsTest, openai_get_header)
{
  ObArray<ObString> headers;
  ObString api_key("sk-test-key");

  ASSERT_EQ(OB_SUCCESS, ObOpenAIUtils::get_header(allocator_, api_key, headers));
  ASSERT_EQ(2, headers.count());
  ASSERT_EQ(ObString("Authorization: Bearer sk-test-key"), headers[0]);
  ASSERT_EQ(ObString("Content-Type: application/json"), headers[1]);
}

TEST_F(ObOpenAIUtilsTest, openai_get_header_without_api_key)
{
  ObArray<ObString> headers;
  ObString api_key;

  ASSERT_EQ(OB_INVALID_ARGUMENT, ObOpenAIUtils::get_header(allocator_, api_key, headers));
  ASSERT_EQ(0, headers.count());
}

TEST_F(ObOpenAIUtilsTest, ollama_get_header_is_empty)
{
  // the ollama service does not accept any http header, an empty header list is legal
  ObArray<ObString> headers;

  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::get_header(allocator_, headers));
  ASSERT_EQ(0, headers.count());
}

TEST_F(ObOpenAIUtilsTest, openai_complete_get_body)
{
  ObOpenAIUtils::ObOpenAIComplete complete;
  ObString model("qwen-plus");
  ObString prompt("you are a helpful assistant");
  ObString content("what is the capital of france");
  ObJsonObject *config = NULL;
  ObJsonObject *body = NULL;

  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, prompt, content, config, body));
  ASSERT_TRUE(!OB_ISNULL(body));

  ObJsonNode *model_node = body->get_value("model");
  ASSERT_TRUE(!OB_ISNULL(model_node));
  ASSERT_EQ(ObJsonNodeType::J_STRING, model_node->json_type());
  ASSERT_EQ(ObString("qwen-plus"), static_cast<ObJsonString *>(model_node)->get_str());

  ObJsonNode *messages_node = body->get_value("messages");
  ASSERT_TRUE(!OB_ISNULL(messages_node));
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, messages_node->json_type());
  ObJsonArray *messages = static_cast<ObJsonArray *>(messages_node);
  // the system prompt and the user content
  ASSERT_EQ(2, messages->element_count());

  ObJsonObject *system_message = static_cast<ObJsonObject *>(messages->get_value(0));
  ASSERT_TRUE(!OB_ISNULL(system_message));
  ASSERT_EQ(ObString("system"),
            static_cast<ObJsonString *>(system_message->get_value("role"))->get_str());
  ASSERT_EQ(ObString("you are a helpful assistant"),
            static_cast<ObJsonString *>(system_message->get_value("content"))->get_str());

  ObJsonObject *user_message = static_cast<ObJsonObject *>(messages->get_value(1));
  ASSERT_TRUE(!OB_ISNULL(user_message));
  ASSERT_EQ(ObString("user"),
            static_cast<ObJsonString *>(user_message->get_value("role"))->get_str());
  ASSERT_EQ(ObString("what is the capital of france"),
            static_cast<ObJsonString *>(user_message->get_value("content"))->get_str());
}

TEST_F(ObOpenAIUtilsTest, openai_complete_get_body_without_prompt)
{
  ObOpenAIUtils::ObOpenAIComplete complete;
  ObString model("qwen-plus");
  ObString prompt("");
  ObString content("hello");
  ObJsonObject *config = NULL;
  ObJsonObject *body = NULL;

  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, prompt, content, config, body));
  ASSERT_TRUE(!OB_ISNULL(body));

  ObJsonArray *messages = static_cast<ObJsonArray *>(body->get_value("messages"));
  ASSERT_TRUE(!OB_ISNULL(messages));
  // only the user message, there is no system prompt
  ASSERT_EQ(1, messages->element_count());
  ASSERT_EQ(ObString("user"),
            static_cast<ObJsonString *>(
                static_cast<ObJsonObject *>(messages->get_value(0))->get_value("role"))->get_str());
}

TEST_F(ObOpenAIUtilsTest, openai_complete_get_body_rejects_empty_content)
{
  ObOpenAIUtils::ObOpenAIComplete complete;
  ObString model("qwen-plus");
  ObString prompt("prompt");
  ObString content("");
  ObJsonObject *config = NULL;
  ObJsonObject *body = NULL;

  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ASSERT_EQ(OB_INVALID_ARGUMENT, complete.get_body(allocator_, model, prompt, content, config, body));
  ASSERT_TRUE(OB_ISNULL(body));
}

TEST_F(ObOpenAIUtilsTest, openai_complete_get_body_merges_config)
{
  ObOpenAIUtils::ObOpenAIComplete complete;
  ObString model("qwen-plus");
  ObString prompt("");
  ObString content("hello");
  ObJsonObject *config = NULL;
  ObJsonObject *body = NULL;

  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ObJsonInt *temperature = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_int(allocator_, 1, temperature));
  ASSERT_EQ(OB_SUCCESS, config->add("temperature", temperature));

  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, prompt, content, config, body));
  ASSERT_TRUE(!OB_ISNULL(body));
  ASSERT_TRUE(!OB_ISNULL(body->get_value("temperature")));
}

TEST_F(ObOpenAIUtilsTest, openai_complete_parse_output)
{
  ObOpenAIUtils::ObOpenAIComplete complete;
  ObJsonObject *response = NULL;
  ObIJsonBase *result = NULL;

  ASSERT_EQ(OB_SUCCESS, parse_json_object(
                            "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"paris\"}}]}",
                            response));
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator_, response, result));
  ASSERT_TRUE(!OB_ISNULL(result));
  ASSERT_EQ(ObJsonNodeType::J_STRING, result->json_type());
  ASSERT_EQ(ObString("paris"), static_cast<ObJsonString *>(result)->get_str());
}

TEST_F(ObOpenAIUtilsTest, openai_complete_parse_output_rejects_invalid_response)
{
  ObOpenAIUtils::ObOpenAIComplete complete;
  ObJsonObject *response = NULL;
  ObIJsonBase *result = NULL;

  ASSERT_EQ(OB_SUCCESS, parse_json_object("{\"choices\":[]}", response));
  ASSERT_EQ(OB_INVALID_DATA, complete.parse_output(allocator_, response, result));
  ASSERT_TRUE(OB_ISNULL(result));
}

TEST_F(ObOpenAIUtilsTest, openai_embed_get_body)
{
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObString model("text-embedding-v1");
  ObArray<ObString> contents;
  ObJsonObject *config = NULL;
  ObJsonObject *body = NULL;

  ObString first("hello");
  ObString second("world");
  ASSERT_EQ(OB_SUCCESS, contents.push_back(first));
  ASSERT_EQ(OB_SUCCESS, contents.push_back(second));

  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ASSERT_EQ(OB_SUCCESS, embed.get_body(allocator_, model, contents, config, body));
  ASSERT_TRUE(!OB_ISNULL(body));
  ASSERT_EQ(ObString("text-embedding-v1"),
            static_cast<ObJsonString *>(body->get_value("model"))->get_str());

  ObJsonArray *input = static_cast<ObJsonArray *>(body->get_value("input"));
  ASSERT_TRUE(!OB_ISNULL(input));
  ASSERT_EQ(2, input->element_count());
  ASSERT_EQ(ObString("hello"), static_cast<ObJsonString *>(input->get_value(0))->get_str());
  ASSERT_EQ(ObString("world"), static_cast<ObJsonString *>(input->get_value(1))->get_str());
}

TEST_F(ObOpenAIUtilsTest, openai_embed_get_body_rejects_empty_contents)
{
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObString model("text-embedding-v1");
  ObArray<ObString> contents;
  ObJsonObject *config = NULL;
  ObJsonObject *body = NULL;

  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ASSERT_EQ(OB_INVALID_ARGUMENT, embed.get_body(allocator_, model, contents, config, body));
  ASSERT_TRUE(OB_ISNULL(body));
}

TEST_F(ObOpenAIUtilsTest, openai_embed_parse_output)
{
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObJsonObject *response = NULL;
  ObIJsonBase *result = NULL;

  ASSERT_EQ(OB_SUCCESS, parse_json_object(
                            "{\"data\":[{\"embedding\":[0.1,0.2]},{\"embedding\":[0.3,0.4]}]}",
                            response));
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator_, response, result));
  ASSERT_TRUE(!OB_ISNULL(result));
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  ObJsonArray *embeddings = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, embeddings->element_count());
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, embeddings->get_value(0)->json_type());
}

TEST_F(ObOpenAIUtilsTest, ollama_complete_get_body)
{
  ObOllamaUtils::ObOllamaComplete complete;
  ObString model("qwen2.5");
  ObString prompt("you are a helpful assistant");
  ObString content("hello");
  ObJsonObject *config = NULL;
  ObJsonObject *body = NULL;

  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  // the ollama service accepts the stream option, it is merged into the body
  ObJsonBoolean *stream = NULL;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_boolean(allocator_, false, stream));
  ASSERT_EQ(OB_SUCCESS, config->add("stream", stream));

  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, prompt, content, config, body));
  ASSERT_TRUE(!OB_ISNULL(body));
  ASSERT_EQ(ObString("qwen2.5"),
            static_cast<ObJsonString *>(body->get_value("model"))->get_str());
  ASSERT_EQ(ObString("hello"),
            static_cast<ObJsonString *>(body->get_value("prompt"))->get_str());
  ASSERT_TRUE(!OB_ISNULL(body->get_value("stream")));
}

TEST_F(ObOpenAIUtilsTest, ollama_complete_parse_output)
{
  ObOllamaUtils::ObOllamaComplete complete;
  ObJsonObject *response = NULL;
  ObIJsonBase *result = NULL;

  ASSERT_EQ(OB_SUCCESS, parse_json_object("{\"response\":\"mock:ollama\"}", response));
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator_, response, result));
  ASSERT_TRUE(!OB_ISNULL(result));
  ASSERT_EQ(ObJsonNodeType::J_STRING, result->json_type());
  ASSERT_EQ(ObString("mock:ollama"), static_cast<ObJsonString *>(result)->get_str());
}

} // namespace
