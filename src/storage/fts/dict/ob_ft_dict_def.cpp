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

#include "storage/fts/dict/ob_ft_dict_def.h"

namespace oceanbase
{
namespace storage
{

common::ObString ObFTSingleWord::get_word() const { return ObString(word_len, word); }

uint64_t ObFTSingleWord::hash(const char *word, int32_t word_len)
{
  constexpr uint64_t FNV_OFFSET_BASIS = 1469598103934665603ULL;
  constexpr uint64_t FNV_PRIME = 1099511628211ULL;
  uint64_t hash_val = FNV_OFFSET_BASIS ^ static_cast<uint64_t>(word_len);
  for (int32_t i = 0; i < word_len; ++i) {
    hash_val ^= static_cast<unsigned char>(word[i]);
    hash_val *= FNV_PRIME;
  }
  return hash_val;
}

uint64_t ObFTSingleWord::hash() const
{
  return hash(word, word_len);
}

bool ObFTSingleWord::operator==(const ObFTSingleWord &other) const
{
  return (this == &other)
         || (word_len == other.word_len && 0 == memcmp(word, other.word, word_len));
}

int32_t ObFTSingleWord::set_word(const char *word, int32_t word_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(word)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (word_len > ObCharset::MAX_MB_LEN) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    memcpy(this->word, word, word_len);
    this->word_len = word_len;
  }
  return ret;
}

} //  namespace storage
} //  namespace oceanbase
