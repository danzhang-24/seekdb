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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(shared_dict_map_.create(K_MAX_DICT_BUCKET, "shared_dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  is_inited_ = false;
  // release the kv cache handles of the shared dictionaries, the memory of the
  // dictionaries themselves is released by the allocator
  for (hash::ObHashMap<ObFTDictInfoKey, ObFTSharedDict>::iterator iter = shared_dict_map_.begin();
       OB_SUCC(ret) && iter != shared_dict_map_.end();
       ++iter) {
    if (OB_NOT_NULL(iter->second.container_)) {
      iter->second.container_->reset();
    }
  }
  shared_dict_map_.destroy();
  shared_dict_alloc_.reset();
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTSharedDict shared;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(shared_dict_map_.get_refactored(key, shared))) {
    if (OB_HASH_NOT_EXIST == ret) {
      ret = build_shared_dict(desc, key, dict);
    }
  } else if (OB_ISNULL(shared.dict_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    dict = shared.dict_;
  }

  return ret;
}

int ObFTDictHub::build_shared_dict(const ObFTDictDesc &desc,
                                   const ObFTDictInfoKey &key,
                                   ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  ObFTCacheRangeContainer *container = nullptr;
  ObFTRangeDict *range_dict = nullptr;
  ObFTSharedDict shared;

  // only one thread builds the dictionary, the others wait for it and then use
  // the dictionary that has been built
  lib::ObMutexGuard guard(shared_dict_lock_);
  if (OB_SUCC(shared_dict_map_.get_refactored(key, shared)) && OB_NOT_NULL(shared.dict_)) {
    dict = shared.dict_;
    return OB_SUCCESS;
  }

  if (OB_ISNULL(container = OB_NEWx(ObFTCacheRangeContainer, &shared_dict_alloc_, shared_dict_alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(load_cache(desc, *container))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
      if (OB_FAIL(build_cache(desc, *container))) {
      }
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_ISNULL(range_dict = OB_NEWx(ObFTRangeDict, &shared_dict_alloc_, shared_dict_alloc_, container, desc))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(range_dict->init())) {
      ret = OB_ERR_UNEXPECTED;
    }
  }

  if (OB_SUCC(ret)) {
    shared.dict_ = range_dict;
    shared.container_ = container;
    if (OB_FAIL(shared_dict_map_.set_refactored(key, shared))) {
    } else {
      dict = range_dict;
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
