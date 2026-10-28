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

#ifndef OB_SPARSE_RETRIEVAL_UTIL_H_
#define OB_SPARSE_RETRIEVAL_UTIL_H_

#include "common/datum/ob_datum.h"
#include "share/datum/ob_datum_funcs.h"

namespace oceanbase
{
namespace storage
{

inline int compare_binary_datum(const ObDatum &lhs, const ObDatum &rhs)
{
  const ObString &lhs_str = lhs.get_string();
  const ObString &rhs_str = rhs.get_string();
  if (OB_LIKELY(16 == lhs_str.length() && 16 == rhs_str.length())) {
    uint64_t lhs_high = 0;
    uint64_t lhs_low = 0;
    uint64_t rhs_high = 0;
    uint64_t rhs_low = 0;
    MEMCPY(&lhs_high, lhs_str.ptr(), sizeof(lhs_high));
    MEMCPY(&lhs_low, lhs_str.ptr() + sizeof(lhs_high), sizeof(lhs_low));
    MEMCPY(&rhs_high, rhs_str.ptr(), sizeof(rhs_high));
    MEMCPY(&rhs_low, rhs_str.ptr() + sizeof(rhs_high), sizeof(rhs_low));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    lhs_high = __builtin_bswap64(lhs_high);
    lhs_low = __builtin_bswap64(lhs_low);
    rhs_high = __builtin_bswap64(rhs_high);
    rhs_low = __builtin_bswap64(rhs_low);
#endif
    if (lhs_high != rhs_high) {
      return lhs_high < rhs_high ? -1 : 1;
    }
    if (lhs_low != rhs_low) {
      return lhs_low < rhs_low ? -1 : 1;
    }
    return 0;
  }
  const int64_t min_len = MIN(lhs_str.length(), rhs_str.length());
  int cmp_ret = MEMCMP(lhs_str.ptr(), rhs_str.ptr(), min_len);
  if (0 == cmp_ret) {
    cmp_ret = static_cast<int>(lhs_str.length() - rhs_str.length());
  }
  return cmp_ret;
}

class ObDomainIdCmp
{
public:
  ObDomainIdCmp() : cmp_func_(nullptr), is_binary_(false) {}
  ~ObDomainIdCmp() {}
  inline int init(const ObObjMeta &obj_meta);
  inline int compare(const ObDatum &lhs, const ObDatum &rhs, int &cmp_ret) const;
  inline void reset() { cmp_func_ = nullptr; is_binary_ = false; }
private:
  common::ObDatumCmpFuncType cmp_func_;
  bool is_binary_;
  DISALLOW_COPY_AND_ASSIGN(ObDomainIdCmp);
};

int ObDomainIdCmp::init(const ObObjMeta &obj_meta)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!obj_meta.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN,"invalid argument", K(ret), K(obj_meta));
  } else {
    common::ObDatumBasicFuncs *id_basic_funcs = ObDatumFuncs::get_basic_func(
      obj_meta.get_type(), obj_meta.get_collation_type());
    if (OB_ISNULL(id_basic_funcs)) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN,"failed to get basic functions", K(ret), K(obj_meta));
    } else {
      cmp_func_ = id_basic_funcs->null_first_cmp_;
      is_binary_ = ob_is_string_type(obj_meta.get_type())
          && CS_TYPE_BINARY == obj_meta.get_collation_type();
    }
  }
  return ret;
}

int ObDomainIdCmp::compare(const ObDatum &lhs, const ObDatum &rhs, int &cmp_ret) const
{
  int ret = OB_SUCCESS;
  const bool lmax = lhs.is_max();
  const bool lmin = lhs.is_min();
  const bool rmax = rhs.is_max();
  const bool rmin = rhs.is_min();
  if (OB_UNLIKELY(nullptr == cmp_func_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN,"not init", K(ret));
  } else if (lhs.is_ext() || rhs.is_ext()) {
    if (OB_UNLIKELY(!(lmax || lmin || rmax || rmin))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN,"unexpected ext datum", K(ret), K(lhs), K(rhs));
    } else if (lmax || rmax) {
      cmp_ret = lmax - rmax;
    } else if (lmin || rmin) {
      cmp_ret = -(lmin - rmin);
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (is_binary_ && !lhs.is_null() && !rhs.is_null()) {
    cmp_ret = compare_binary_datum(lhs, rhs);
  } else {
    ret = cmp_func_(lhs, rhs, cmp_ret, nullptr);
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase

#endif
