/*
 * Copyright (c) 2016-2022 INRIA, CMU and Microsoft Corporation
 * Copyright (c) 2022-2023 HACL* Contributors
 * Copyright (c) 2023 Cryspen
 */

#ifndef CRYPTO_HACL_BIGNUM_H_
#define CRYPTO_HACL_BIGNUM_H_

#include "hacl_lib.h"

static inline uint32_t
Hacl_Bignum_Base_mul_wide_add2_u32(uint32_t a, uint32_t b, uint32_t c_in, uint32_t *out)
{
  uint32_t out0 = out[0U];
  uint64_t res = (uint64_t)a * (uint64_t)b + (uint64_t)c_in + (uint64_t)out0;
  out[0U] = (uint32_t)res;
  return (uint32_t)(res >> 32U);
}

static inline uint64_t
Hacl_Bignum_Base_mul_wide_add2_u64(uint64_t a, uint64_t b, uint64_t c_in, uint64_t *out)
{
  uint64_t out0 = out[0U];
  FStar_UInt128_uint128
  res =
    FStar_UInt128_add(FStar_UInt128_add(FStar_UInt128_mul_wide(a, b),
        FStar_UInt128_uint64_to_uint128(c_in)),
      FStar_UInt128_uint64_to_uint128(out0));
  out[0U] = FStar_UInt128_uint128_to_uint64(res);
  return FStar_UInt128_uint128_to_uint64(FStar_UInt128_shift_right(res, 64U));
}

static inline void
Hacl_Bignum_Convert_bn_from_bytes_be_uint64(uint32_t len, uint8_t *b, uint64_t *res)
{
  uint32_t bnLen = (len - 1U) / 8U + 1U;
  uint32_t tmpLen = 8U * bnLen;
  KRML_CHECK_SIZE(sizeof (uint8_t), tmpLen);
  uint8_t *tmp = (uint8_t *)alloca(tmpLen * sizeof (uint8_t));
  memset(tmp, 0U, tmpLen * sizeof (uint8_t));
  memcpy(tmp + tmpLen - len, b, len * sizeof (uint8_t));
  for (uint32_t i = 0U; i < bnLen; i++)
  {
    uint64_t *os = res;
    uint64_t u = load64_be(tmp + (bnLen - i - 1U) * 8U);
    uint64_t x = u;
    os[i] = x;
  }
}

static inline void
Hacl_Bignum_Convert_bn_to_bytes_be_uint64(uint32_t len, uint64_t *b, uint8_t *res)
{
  uint32_t bnLen = (len - 1U) / 8U + 1U;
  uint32_t tmpLen = 8U * bnLen;
  KRML_CHECK_SIZE(sizeof (uint8_t), tmpLen);
  uint8_t *tmp = (uint8_t *)alloca(tmpLen * sizeof (uint8_t));
  memset(tmp, 0U, tmpLen * sizeof (uint8_t));
  for (uint32_t i = 0U; i < bnLen; i++)
  {
    store64_be(tmp + i * 8U, b[bnLen - i - 1U]);
  }
  memcpy(res, tmp + tmpLen - len, len * sizeof (uint8_t));
}

static inline uint32_t Hacl_Bignum_Lib_bn_get_top_index_u32(uint32_t len, uint32_t *b)
{
  uint32_t priv = 0U;
  for (uint32_t i = 0U; i < len; i++)
  {
    uint32_t mask = FStar_UInt32_eq_mask(b[i], 0U);
    priv = (mask & priv) | (~mask & i);
  }
  return priv;
}

static inline uint64_t Hacl_Bignum_Lib_bn_get_top_index_u64(uint32_t len, uint64_t *b)
{
  uint64_t priv = 0ULL;
  for (uint32_t i = 0U; i < len; i++)
  {
    uint64_t mask = FStar_UInt64_eq_mask(b[i], 0ULL);
    priv = (mask & priv) | (~mask & (uint64_t)i);
  }
  return priv;
}

static inline uint32_t
Hacl_Bignum_Lib_bn_get_bits_u32(uint32_t len, uint32_t *b, uint32_t i, uint32_t l)
{
  uint32_t i1 = i / 32U;
  uint32_t j = i % 32U;
  uint32_t p1 = b[i1] >> j;
  uint32_t ite;
  if (i1 + 1U < len && 0U < j)
  {
    ite = p1 | b[i1 + 1U] << (32U - j);
  }
  else
  {
    ite = p1;
  }
  return ite & ((1U << l) - 1U);
}

static inline uint64_t
Hacl_Bignum_Lib_bn_get_bits_u64(uint32_t len, uint64_t *b, uint32_t i, uint32_t l)
{
  uint32_t i1 = i / 64U;
  uint32_t j = i % 64U;
  uint64_t p1 = b[i1] >> j;
  uint64_t ite;
  if (i1 + 1U < len && 0U < j)
  {
    ite = p1 | b[i1 + 1U] << (64U - j);
  }
  else
  {
    ite = p1;
  }
  return ite & ((1ULL << l) - 1ULL);
}

static inline uint32_t
Hacl_Bignum_Addition_bn_sub_eq_len_u32(uint32_t aLen, uint32_t *a, uint32_t *b, uint32_t *res)
{
  uint32_t c = 0U;
  for (uint32_t i = 0U; i < aLen / 4U; i++)
  {
    uint32_t t1 = a[4U * i];
    uint32_t t20 = b[4U * i];
    uint32_t *res_i0 = res + 4U * i;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u32(c, t1, t20, res_i0);
    uint32_t t10 = a[4U * i + 1U];
    uint32_t t21 = b[4U * i + 1U];
    uint32_t *res_i1 = res + 4U * i + 1U;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u32(c, t10, t21, res_i1);
    uint32_t t11 = a[4U * i + 2U];
    uint32_t t22 = b[4U * i + 2U];
    uint32_t *res_i2 = res + 4U * i + 2U;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u32(c, t11, t22, res_i2);
    uint32_t t12 = a[4U * i + 3U];
    uint32_t t2 = b[4U * i + 3U];
    uint32_t *res_i = res + 4U * i + 3U;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u32(c, t12, t2, res_i);
  }
  for (uint32_t i = aLen / 4U * 4U; i < aLen; i++)
  {
    uint32_t t1 = a[i];
    uint32_t t2 = b[i];
    uint32_t *res_i = res + i;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u32(c, t1, t2, res_i);
  }
  return c;
}

static inline uint64_t
Hacl_Bignum_Addition_bn_sub_eq_len_u64(uint32_t aLen, uint64_t *a, uint64_t *b, uint64_t *res)
{
  uint64_t c = 0ULL;
  for (uint32_t i = 0U; i < aLen / 4U; i++)
  {
    uint64_t t1 = a[4U * i];
    uint64_t t20 = b[4U * i];
    uint64_t *res_i0 = res + 4U * i;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u64(c, t1, t20, res_i0);
    uint64_t t10 = a[4U * i + 1U];
    uint64_t t21 = b[4U * i + 1U];
    uint64_t *res_i1 = res + 4U * i + 1U;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u64(c, t10, t21, res_i1);
    uint64_t t11 = a[4U * i + 2U];
    uint64_t t22 = b[4U * i + 2U];
    uint64_t *res_i2 = res + 4U * i + 2U;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u64(c, t11, t22, res_i2);
    uint64_t t12 = a[4U * i + 3U];
    uint64_t t2 = b[4U * i + 3U];
    uint64_t *res_i = res + 4U * i + 3U;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u64(c, t12, t2, res_i);
  }
  for (uint32_t i = aLen / 4U * 4U; i < aLen; i++)
  {
    uint64_t t1 = a[i];
    uint64_t t2 = b[i];
    uint64_t *res_i = res + i;
    c = Lib_IntTypes_Intrinsics_sub_borrow_u64(c, t1, t2, res_i);
  }
  return c;
}

static inline uint32_t
Hacl_Bignum_Addition_bn_add_eq_len_u32(uint32_t aLen, uint32_t *a, uint32_t *b, uint32_t *res)
{
  uint32_t c = 0U;
  for (uint32_t i = 0U; i < aLen / 4U; i++)
  {
    uint32_t t1 = a[4U * i];
    uint32_t t20 = b[4U * i];
    uint32_t *res_i0 = res + 4U * i;
    c = Lib_IntTypes_Intrinsics_add_carry_u32(c, t1, t20, res_i0);
    uint32_t t10 = a[4U * i + 1U];
    uint32_t t21 = b[4U * i + 1U];
    uint32_t *res_i1 = res + 4U * i + 1U;
    c = Lib_IntTypes_Intrinsics_add_carry_u32(c, t10, t21, res_i1);
    uint32_t t11 = a[4U * i + 2U];
    uint32_t t22 = b[4U * i + 2U];
    uint32_t *res_i2 = res + 4U * i + 2U;
    c = Lib_IntTypes_Intrinsics_add_carry_u32(c, t11, t22, res_i2);
    uint32_t t12 = a[4U * i + 3U];
    uint32_t t2 = b[4U * i + 3U];
    uint32_t *res_i = res + 4U * i + 3U;
    c = Lib_IntTypes_Intrinsics_add_carry_u32(c, t12, t2, res_i);
  }
  for (uint32_t i = aLen / 4U * 4U; i < aLen; i++)
  {
    uint32_t t1 = a[i];
    uint32_t t2 = b[i];
    uint32_t *res_i = res + i;
    c = Lib_IntTypes_Intrinsics_add_carry_u32(c, t1, t2, res_i);
  }
  return c;
}

static inline uint64_t
Hacl_Bignum_Addition_bn_add_eq_len_u64(uint32_t aLen, uint64_t *a, uint64_t *b, uint64_t *res)
{
  uint64_t c = 0ULL;
  for (uint32_t i = 0U; i < aLen / 4U; i++)
  {
    uint64_t t1 = a[4U * i];
    uint64_t t20 = b[4U * i];
    uint64_t *res_i0 = res + 4U * i;
    c = Lib_IntTypes_Intrinsics_add_carry_u64(c, t1, t20, res_i0);
    uint64_t t10 = a[4U * i + 1U];
    uint64_t t21 = b[4U * i + 1U];
    uint64_t *res_i1 = res + 4U * i + 1U;
    c = Lib_IntTypes_Intrinsics_add_carry_u64(c, t10, t21, res_i1);
    uint64_t t11 = a[4U * i + 2U];
    uint64_t t22 = b[4U * i + 2U];
    uint64_t *res_i2 = res + 4U * i + 2U;
    c = Lib_IntTypes_Intrinsics_add_carry_u64(c, t11, t22, res_i2);
    uint64_t t12 = a[4U * i + 3U];
    uint64_t t2 = b[4U * i + 3U];
    uint64_t *res_i = res + 4U * i + 3U;
    c = Lib_IntTypes_Intrinsics_add_carry_u64(c, t12, t2, res_i);
  }
  for (uint32_t i = aLen / 4U * 4U; i < aLen; i++)
  {
    uint64_t t1 = a[i];
    uint64_t t2 = b[i];
    uint64_t *res_i = res + i;
    c = Lib_IntTypes_Intrinsics_add_carry_u64(c, t1, t2, res_i);
  }
  return c;
}

static inline void
Hacl_Bignum_Multiplication_bn_mul_u32(
  uint32_t aLen,
  uint32_t *a,
  uint32_t bLen,
  uint32_t *b,
  uint32_t *res
)
{
  memset(res, 0U, (aLen + bLen) * sizeof (uint32_t));
  for (uint32_t i0 = 0U; i0 < bLen; i0++)
  {
    uint32_t bj = b[i0];
    uint32_t *res_j = res + i0;
    uint32_t c = 0U;
    for (uint32_t i = 0U; i < aLen / 4U; i++)
    {
      uint32_t a_i = a[4U * i];
      uint32_t *res_i0 = res_j + 4U * i;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i, bj, c, res_i0);
      uint32_t a_i0 = a[4U * i + 1U];
      uint32_t *res_i1 = res_j + 4U * i + 1U;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i0, bj, c, res_i1);
      uint32_t a_i1 = a[4U * i + 2U];
      uint32_t *res_i2 = res_j + 4U * i + 2U;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i1, bj, c, res_i2);
      uint32_t a_i2 = a[4U * i + 3U];
      uint32_t *res_i = res_j + 4U * i + 3U;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i2, bj, c, res_i);
    }
    for (uint32_t i = aLen / 4U * 4U; i < aLen; i++)
    {
      uint32_t a_i = a[i];
      uint32_t *res_i = res_j + i;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i, bj, c, res_i);
    }
    uint32_t r = c;
    res[aLen + i0] = r;
  }
}

static inline void
Hacl_Bignum_Multiplication_bn_mul_u64(
  uint32_t aLen,
  uint64_t *a,
  uint32_t bLen,
  uint64_t *b,
  uint64_t *res
)
{
  memset(res, 0U, (aLen + bLen) * sizeof (uint64_t));
  for (uint32_t i0 = 0U; i0 < bLen; i0++)
  {
    uint64_t bj = b[i0];
    uint64_t *res_j = res + i0;
    uint64_t c = 0ULL;
    for (uint32_t i = 0U; i < aLen / 4U; i++)
    {
      uint64_t a_i = a[4U * i];
      uint64_t *res_i0 = res_j + 4U * i;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i, bj, c, res_i0);
      uint64_t a_i0 = a[4U * i + 1U];
      uint64_t *res_i1 = res_j + 4U * i + 1U;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i0, bj, c, res_i1);
      uint64_t a_i1 = a[4U * i + 2U];
      uint64_t *res_i2 = res_j + 4U * i + 2U;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i1, bj, c, res_i2);
      uint64_t a_i2 = a[4U * i + 3U];
      uint64_t *res_i = res_j + 4U * i + 3U;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i2, bj, c, res_i);
    }
    for (uint32_t i = aLen / 4U * 4U; i < aLen; i++)
    {
      uint64_t a_i = a[i];
      uint64_t *res_i = res_j + i;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i, bj, c, res_i);
    }
    uint64_t r = c;
    res[aLen + i0] = r;
  }
}

static inline void
Hacl_Bignum_Multiplication_bn_sqr_u32(uint32_t aLen, uint32_t *a, uint32_t *res)
{
  memset(res, 0U, (aLen + aLen) * sizeof (uint32_t));
  for (uint32_t i0 = 0U; i0 < aLen; i0++)
  {
    uint32_t *ab = a;
    uint32_t a_j = a[i0];
    uint32_t *res_j = res + i0;
    uint32_t c = 0U;
    for (uint32_t i = 0U; i < i0 / 4U; i++)
    {
      uint32_t a_i = ab[4U * i];
      uint32_t *res_i0 = res_j + 4U * i;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i, a_j, c, res_i0);
      uint32_t a_i0 = ab[4U * i + 1U];
      uint32_t *res_i1 = res_j + 4U * i + 1U;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i0, a_j, c, res_i1);
      uint32_t a_i1 = ab[4U * i + 2U];
      uint32_t *res_i2 = res_j + 4U * i + 2U;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i1, a_j, c, res_i2);
      uint32_t a_i2 = ab[4U * i + 3U];
      uint32_t *res_i = res_j + 4U * i + 3U;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i2, a_j, c, res_i);
    }
    for (uint32_t i = i0 / 4U * 4U; i < i0; i++)
    {
      uint32_t a_i = ab[i];
      uint32_t *res_i = res_j + i;
      c = Hacl_Bignum_Base_mul_wide_add2_u32(a_i, a_j, c, res_i);
    }
    uint32_t r = c;
    res[i0 + i0] = r;
  }
  uint32_t c0 = Hacl_Bignum_Addition_bn_add_eq_len_u32(aLen + aLen, res, res, res);
  KRML_MAYBE_UNUSED_VAR(c0);
  KRML_CHECK_SIZE(sizeof (uint32_t), aLen + aLen);
  uint32_t *tmp = (uint32_t *)alloca((aLen + aLen) * sizeof (uint32_t));
  memset(tmp, 0U, (aLen + aLen) * sizeof (uint32_t));
  for (uint32_t i = 0U; i < aLen; i++)
  {
    uint64_t res1 = (uint64_t)a[i] * (uint64_t)a[i];
    uint32_t hi = (uint32_t)(res1 >> 32U);
    uint32_t lo = (uint32_t)res1;
    tmp[2U * i] = lo;
    tmp[2U * i + 1U] = hi;
  }
  uint32_t c1 = Hacl_Bignum_Addition_bn_add_eq_len_u32(aLen + aLen, res, tmp, res);
  KRML_MAYBE_UNUSED_VAR(c1);
}

static inline void
Hacl_Bignum_Multiplication_bn_sqr_u64(uint32_t aLen, uint64_t *a, uint64_t *res)
{
  memset(res, 0U, (aLen + aLen) * sizeof (uint64_t));
  for (uint32_t i0 = 0U; i0 < aLen; i0++)
  {
    uint64_t *ab = a;
    uint64_t a_j = a[i0];
    uint64_t *res_j = res + i0;
    uint64_t c = 0ULL;
    for (uint32_t i = 0U; i < i0 / 4U; i++)
    {
      uint64_t a_i = ab[4U * i];
      uint64_t *res_i0 = res_j + 4U * i;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i, a_j, c, res_i0);
      uint64_t a_i0 = ab[4U * i + 1U];
      uint64_t *res_i1 = res_j + 4U * i + 1U;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i0, a_j, c, res_i1);
      uint64_t a_i1 = ab[4U * i + 2U];
      uint64_t *res_i2 = res_j + 4U * i + 2U;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i1, a_j, c, res_i2);
      uint64_t a_i2 = ab[4U * i + 3U];
      uint64_t *res_i = res_j + 4U * i + 3U;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i2, a_j, c, res_i);
    }
    for (uint32_t i = i0 / 4U * 4U; i < i0; i++)
    {
      uint64_t a_i = ab[i];
      uint64_t *res_i = res_j + i;
      c = Hacl_Bignum_Base_mul_wide_add2_u64(a_i, a_j, c, res_i);
    }
    uint64_t r = c;
    res[i0 + i0] = r;
  }
  uint64_t c0 = Hacl_Bignum_Addition_bn_add_eq_len_u64(aLen + aLen, res, res, res);
  KRML_MAYBE_UNUSED_VAR(c0);
  KRML_CHECK_SIZE(sizeof (uint64_t), aLen + aLen);
  uint64_t *tmp = (uint64_t *)alloca((aLen + aLen) * sizeof (uint64_t));
  memset(tmp, 0U, (aLen + aLen) * sizeof (uint64_t));
  for (uint32_t i = 0U; i < aLen; i++)
  {
    FStar_UInt128_uint128 res1 = FStar_UInt128_mul_wide(a[i], a[i]);
    uint64_t hi = FStar_UInt128_uint128_to_uint64(FStar_UInt128_shift_right(res1, 64U));
    uint64_t lo = FStar_UInt128_uint128_to_uint64(res1);
    tmp[2U * i] = lo;
    tmp[2U * i + 1U] = hi;
  }
  uint64_t c1 = Hacl_Bignum_Addition_bn_add_eq_len_u64(aLen + aLen, res, tmp, res);
  KRML_MAYBE_UNUSED_VAR(c1);
}

typedef struct Hacl_Bignum_MontArithmetic_bn_mont_ctx_u32_s
{
  uint32_t len;
  uint32_t *n;
  uint32_t mu;
  uint32_t *r2;
}
Hacl_Bignum_MontArithmetic_bn_mont_ctx_u32;

typedef struct Hacl_Bignum_MontArithmetic_bn_mont_ctx_u64_s
{
  uint32_t len;
  uint64_t *n;
  uint64_t mu;
  uint64_t *r2;
}
Hacl_Bignum_MontArithmetic_bn_mont_ctx_u64;

void
Hacl_Bignum_Karatsuba_bn_karatsuba_mul_uint32(
  uint32_t aLen,
  uint32_t *a,
  uint32_t *b,
  uint32_t *tmp,
  uint32_t *res
);

void
Hacl_Bignum_Karatsuba_bn_karatsuba_mul_uint64(
  uint32_t aLen,
  uint64_t *a,
  uint64_t *b,
  uint64_t *tmp,
  uint64_t *res
);

void
Hacl_Bignum_Karatsuba_bn_karatsuba_sqr_uint32(
  uint32_t aLen,
  uint32_t *a,
  uint32_t *tmp,
  uint32_t *res
);

void
Hacl_Bignum_Karatsuba_bn_karatsuba_sqr_uint64(
  uint32_t aLen,
  uint64_t *a,
  uint64_t *tmp,
  uint64_t *res
);

void
Hacl_Bignum_bn_add_mod_n_u32(
  uint32_t len1,
  uint32_t *n,
  uint32_t *a,
  uint32_t *b,
  uint32_t *res
);

void
Hacl_Bignum_bn_add_mod_n_u64(
  uint32_t len1,
  uint64_t *n,
  uint64_t *a,
  uint64_t *b,
  uint64_t *res
);

void
Hacl_Bignum_bn_sub_mod_n_u32(
  uint32_t len1,
  uint32_t *n,
  uint32_t *a,
  uint32_t *b,
  uint32_t *res
);

void
Hacl_Bignum_bn_sub_mod_n_u64(
  uint32_t len1,
  uint64_t *n,
  uint64_t *a,
  uint64_t *b,
  uint64_t *res
);

uint32_t Hacl_Bignum_ModInvLimb_mod_inv_uint32(uint32_t n0);

uint64_t Hacl_Bignum_ModInvLimb_mod_inv_uint64(uint64_t n0);

uint32_t Hacl_Bignum_Montgomery_bn_check_modulus_u32(uint32_t len, uint32_t *n);

void
Hacl_Bignum_Montgomery_bn_precomp_r2_mod_n_u32(
  uint32_t len,
  uint32_t nBits,
  uint32_t *n,
  uint32_t *res
);

void
Hacl_Bignum_Montgomery_bn_to_mont_u32(
  uint32_t len,
  uint32_t *n,
  uint32_t nInv,
  uint32_t *r2,
  uint32_t *a,
  uint32_t *aM
);

void
Hacl_Bignum_Montgomery_bn_from_mont_u32(
  uint32_t len,
  uint32_t *n,
  uint32_t nInv_u64,
  uint32_t *aM,
  uint32_t *a
);

void
Hacl_Bignum_Montgomery_bn_mont_mul_u32(
  uint32_t len,
  uint32_t *n,
  uint32_t nInv_u64,
  uint32_t *aM,
  uint32_t *bM,
  uint32_t *resM
);

void
Hacl_Bignum_Montgomery_bn_mont_sqr_u32(
  uint32_t len,
  uint32_t *n,
  uint32_t nInv_u64,
  uint32_t *aM,
  uint32_t *resM
);

uint64_t Hacl_Bignum_Montgomery_bn_check_modulus_u64(uint32_t len, uint64_t *n);

void
Hacl_Bignum_Montgomery_bn_precomp_r2_mod_n_u64(
  uint32_t len,
  uint32_t nBits,
  uint64_t *n,
  uint64_t *res
);

void
Hacl_Bignum_Montgomery_bn_to_mont_u64(
  uint32_t len,
  uint64_t *n,
  uint64_t nInv,
  uint64_t *r2,
  uint64_t *a,
  uint64_t *aM
);

void
Hacl_Bignum_Montgomery_bn_from_mont_u64(
  uint32_t len,
  uint64_t *n,
  uint64_t nInv_u64,
  uint64_t *aM,
  uint64_t *a
);

void
Hacl_Bignum_Montgomery_bn_mont_mul_u64(
  uint32_t len,
  uint64_t *n,
  uint64_t nInv_u64,
  uint64_t *aM,
  uint64_t *bM,
  uint64_t *resM
);

void
Hacl_Bignum_Montgomery_bn_mont_sqr_u64(
  uint32_t len,
  uint64_t *n,
  uint64_t nInv_u64,
  uint64_t *aM,
  uint64_t *resM
);

void
Hacl_Bignum_AlmostMontgomery_bn_almost_mont_reduction_u32(
  uint32_t len,
  uint32_t *n,
  uint32_t nInv,
  uint32_t *c,
  uint32_t *res
);

void
Hacl_Bignum_AlmostMontgomery_bn_almost_mont_reduction_u64(
  uint32_t len,
  uint64_t *n,
  uint64_t nInv,
  uint64_t *c,
  uint64_t *res
);

uint32_t
Hacl_Bignum_Exponentiation_bn_check_mod_exp_u32(
  uint32_t len,
  uint32_t *n,
  uint32_t *a,
  uint32_t bBits,
  uint32_t *b
);

void
Hacl_Bignum_Exponentiation_bn_mod_exp_vartime_precomp_u32(
  uint32_t len,
  uint32_t *n,
  uint32_t mu,
  uint32_t *r2,
  uint32_t *a,
  uint32_t bBits,
  uint32_t *b,
  uint32_t *res
);

void
Hacl_Bignum_Exponentiation_bn_mod_exp_consttime_precomp_u32(
  uint32_t len,
  uint32_t *n,
  uint32_t mu,
  uint32_t *r2,
  uint32_t *a,
  uint32_t bBits,
  uint32_t *b,
  uint32_t *res
);

void
Hacl_Bignum_Exponentiation_bn_mod_exp_vartime_u32(
  uint32_t len,
  uint32_t nBits,
  uint32_t *n,
  uint32_t *a,
  uint32_t bBits,
  uint32_t *b,
  uint32_t *res
);

void
Hacl_Bignum_Exponentiation_bn_mod_exp_consttime_u32(
  uint32_t len,
  uint32_t nBits,
  uint32_t *n,
  uint32_t *a,
  uint32_t bBits,
  uint32_t *b,
  uint32_t *res
);

uint64_t
Hacl_Bignum_Exponentiation_bn_check_mod_exp_u64(
  uint32_t len,
  uint64_t *n,
  uint64_t *a,
  uint32_t bBits,
  uint64_t *b
);

void
Hacl_Bignum_Exponentiation_bn_mod_exp_vartime_precomp_u64(
  uint32_t len,
  uint64_t *n,
  uint64_t mu,
  uint64_t *r2,
  uint64_t *a,
  uint32_t bBits,
  uint64_t *b,
  uint64_t *res
);

void
Hacl_Bignum_Exponentiation_bn_mod_exp_consttime_precomp_u64(
  uint32_t len,
  uint64_t *n,
  uint64_t mu,
  uint64_t *r2,
  uint64_t *a,
  uint32_t bBits,
  uint64_t *b,
  uint64_t *res
);

void
Hacl_Bignum_Exponentiation_bn_mod_exp_vartime_u64(
  uint32_t len,
  uint32_t nBits,
  uint64_t *n,
  uint64_t *a,
  uint32_t bBits,
  uint64_t *b,
  uint64_t *res
);

void
Hacl_Bignum_Exponentiation_bn_mod_exp_consttime_u64(
  uint32_t len,
  uint32_t nBits,
  uint64_t *n,
  uint64_t *a,
  uint32_t bBits,
  uint64_t *b,
  uint64_t *res
);

#endif
