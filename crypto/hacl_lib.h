/*
 * Copyright (c) 2016-2022 INRIA, CMU and Microsoft Corporation
 * Copyright (c) 2022-2023 HACL* Contributors
 * Copyright (c) 2023 Cryspen
 */

#ifndef CRYPTO_HACL_LIB_H_
#define CRYPTO_HACL_LIB_H_

#include <asm/unaligned.h>
#include <crypto/sha256_base.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/string.h>

#define alloca __builtin_alloca

typedef u128 FStar_UInt128_uint128;

static inline u128 FStar_UInt128_shift_left(u128 x, u32 y)
{
        return (x << y);
}

static inline u128 FStar_UInt128_add(u128 x, u128 y)
{
        return (x + y);
}

static inline u128 FStar_UInt128_uint64_to_uint128(u64 x)
{
        return ((u128)x);
}

inline static u128 FStar_UInt128_mul_wide(u64 x, u64 y) {
  return ((u128) x) * y;
}

inline static uint64_t FStar_UInt128_uint128_to_uint64(u128 x) {
  return (u64)x;
}

inline static u128 FStar_UInt128_shift_right(u128 x, u32 y) {
  return x >> y;
}

#define KRML_NOINLINE noinline __maybe_unused
#define KRML_MAYBE_UNUSED_VAR(x) (void)(x)
#define KRML_HOST_CALLOC(x,y) kcalloc(x,y,GFP_KERNEL)
#define KRML_HOST_FREE(x) kfree(x)

static KRML_NOINLINE u32 FStar_UInt32_eq_mask(u32 a, u32 b)
{
  u32 x = a ^ b;
  u32 minus_x = ~x + (u32)1U;
  u32 x_or_minus_x = x | minus_x;
  u32 xnx = x_or_minus_x >> (u32)31U;
  return xnx - (u32)1U;
}

static KRML_NOINLINE u32 FStar_UInt32_gte_mask(u32 a, u32 b)
{
  u32 x = a;
  u32 y = b;
  u32 x_xor_y = x ^ y;
  u32 x_sub_y = x - y;
  u32 x_sub_y_xor_y = x_sub_y ^ y;
  u32 q = x_xor_y | x_sub_y_xor_y;
  u32 x_xor_q = x ^ q;
  u32 x_xor_q_ = x_xor_q >> (u32)31U;
  return x_xor_q_ - (u32)1U;
}


static KRML_NOINLINE u64 FStar_UInt64_eq_mask(u64 a, u64 b)
{
  u64 x = a ^ b;
  u64 minus_x = ~x + (u64)1U;
  u64 x_or_minus_x = x | minus_x;
  u64 xnx = x_or_minus_x >> (u32)63U;
  return xnx - (u64)1U;
}

static KRML_NOINLINE u64 FStar_UInt64_gte_mask(u64 a, u64 b)
{
  u64 x = a;
  u64 y = b;
  u64 x_xor_y = x ^ y;
  u64 x_sub_y = x - y;
  u64 x_sub_y_xor_y = x_sub_y ^ y;
  u64 q = x_xor_y | x_sub_y_xor_y;
  u64 x_xor_q = x ^ q;
  u64 x_xor_q_ = x_xor_q >> (u32)63U;
  return x_xor_q_ - (u64)1U;
}

static inline uint32_t
Hacl_IntTypes_Intrinsics_add_carry_u32(uint32_t cin, uint32_t x, uint32_t y, uint32_t *r)
{
  uint64_t res = (uint64_t)x + (uint64_t)cin + (uint64_t)y;
  uint32_t c = (uint32_t)(res >> 32U);
  r[0U] = (uint32_t)res;
  return c;
}

static inline uint32_t
Hacl_IntTypes_Intrinsics_sub_borrow_u32(uint32_t cin, uint32_t x, uint32_t y, uint32_t *r)
{
  uint64_t res = (uint64_t)x - (uint64_t)y - (uint64_t)cin;
  uint32_t c = (uint32_t)(res >> 32U) & 1U;
  r[0U] = (uint32_t)res;
  return c;
}


static inline uint64_t
Hacl_IntTypes_Intrinsics_sub_borrow_u64(uint64_t cin, uint64_t x, uint64_t y, uint64_t *r)
{
  uint64_t res = x - y - cin;
  uint64_t
  c =
    ((FStar_UInt64_gte_mask(res, x) & ~FStar_UInt64_eq_mask(res, x))
    | (FStar_UInt64_eq_mask(res, x) & cin))
    & (uint64_t)1U;
  r[0U] = res;
  return c;
}

static inline uint64_t
Hacl_IntTypes_Intrinsics_add_carry_u64(uint64_t cin, uint64_t x, uint64_t y, uint64_t *r)
{
  uint64_t res = x + cin + y;
  uint64_t
  c = (~FStar_UInt64_gte_mask(res, x) | (FStar_UInt64_eq_mask(res, x) & cin)) & (uint64_t)1U;
  r[0U] = res;
  return c;
}

#define Lib_IntTypes_Intrinsics_sub_borrow_u32(x1, x2, x3, x4) \
    (Hacl_IntTypes_Intrinsics_sub_borrow_u32(x1, x2, x3, x4))
    
#define Lib_IntTypes_Intrinsics_add_carry_u32(x1, x2, x3, x4) \
    (Hacl_IntTypes_Intrinsics_add_carry_u32(x1, x2, x3, x4))

#define Lib_IntTypes_Intrinsics_sub_borrow_u64(x1, x2, x3, x4) \
    (Hacl_IntTypes_Intrinsics_sub_borrow_u64(x1, x2, x3, x4))
    
#define Lib_IntTypes_Intrinsics_add_carry_u64(x1, x2, x3, x4) \
    (Hacl_IntTypes_Intrinsics_add_carry_u64(x1, x2, x3, x4))

/*
 * Loads and stores. These avoid undefined behavior due to unaligned memory
 * accesses, via memcpy.
 */

#define load32_be(b)     (get_unaligned_be32(b))
#define store32_be(b, i) put_unaligned_be32(i, b);
#define load64_be(b)     (get_unaligned_be64(b))
#define store64_be(b, i) put_unaligned_be64(i, b);

#define load32_le(b)     (get_unaligned_le32(b))
#define store32_le(b, i) put_unaligned_le32(i, b);
#define load64_le(b)     (get_unaligned_le64(b))
#define store64_le(b, i) put_unaligned_le64(i, b);

static inline void store128_be(u8 *buf, u128 x)
{
        store64_be(buf, (u64)(x >> 64));
        store64_be(buf + 8, (u64)(x));
}

#define KRML_CHECK_SIZE(size_elt, sz) {}

/* Macros for prettier unrolling of loops */
#define KRML_LOOP1(i, n, x) \
        {                   \
                x i += n;   \
        }

#define KRML_LOOP2(i, n, x) \
        KRML_LOOP1(i, n, x) \
        KRML_LOOP1(i, n, x)

#define KRML_LOOP3(i, n, x) \
        KRML_LOOP2(i, n, x) \
        KRML_LOOP1(i, n, x)

#define KRML_LOOP4(i, n, x) \
        KRML_LOOP2(i, n, x) \
        KRML_LOOP2(i, n, x)

#define KRML_LOOP5(i, n, x) \
        KRML_LOOP4(i, n, x) \
        KRML_LOOP1(i, n, x)

#define KRML_LOOP6(i, n, x) \
        KRML_LOOP4(i, n, x) \
        KRML_LOOP2(i, n, x)

#define KRML_LOOP7(i, n, x) \
        KRML_LOOP4(i, n, x) \
        KRML_LOOP3(i, n, x)

#define KRML_LOOP8(i, n, x) \
        KRML_LOOP4(i, n, x) \
        KRML_LOOP4(i, n, x)

#define KRML_LOOP9(i, n, x) \
        KRML_LOOP8(i, n, x) \
        KRML_LOOP1(i, n, x)

#define KRML_LOOP10(i, n, x) \
        KRML_LOOP8(i, n, x)  \
        KRML_LOOP2(i, n, x)

#define KRML_LOOP11(i, n, x) \
        KRML_LOOP8(i, n, x)  \
        KRML_LOOP3(i, n, x)

#define KRML_LOOP12(i, n, x) \
        KRML_LOOP8(i, n, x)  \
        KRML_LOOP4(i, n, x)

#define KRML_LOOP13(i, n, x) \
        KRML_LOOP8(i, n, x)  \
        KRML_LOOP5(i, n, x)

#define KRML_LOOP14(i, n, x) \
        KRML_LOOP8(i, n, x)  \
        KRML_LOOP6(i, n, x)

#define KRML_LOOP15(i, n, x) \
        KRML_LOOP8(i, n, x)  \
        KRML_LOOP7(i, n, x)

#define KRML_LOOP16(i, n, x) \
        KRML_LOOP8(i, n, x)  \
        KRML_LOOP8(i, n, x)

#define KRML_UNROLL_FOR(i, z, n, k, x) \
        do {                           \
                u32 i = z;        \
                KRML_LOOP##n(i, k, x)  \
        } while (0)

#define KRML_ACTUAL_FOR(i, z, n, k, x)                \
        do {                                          \
                for (u32 i = z; i < n; i += k) { \
                        x                             \
                }                                     \
        } while (0)

#define KRML_UNROLL_MAX 16

/* 1 is the number of loop iterations, i.e. (n - z)/k as evaluated by krml */
#if 0 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR0(i, z, n, k, x)
#else
#define KRML_MAYBE_FOR0(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 1 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR1(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 1, k, x)
#else
#define KRML_MAYBE_FOR1(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 2 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR2(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 2, k, x)
#else
#define KRML_MAYBE_FOR2(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 3 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR3(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 3, k, x)
#else
#define KRML_MAYBE_FOR3(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 4 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR4(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 4, k, x)
#else
#define KRML_MAYBE_FOR4(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 5 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR5(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 5, k, x)
#else
#define KRML_MAYBE_FOR5(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 6 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR6(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 6, k, x)
#else
#define KRML_MAYBE_FOR6(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 7 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR7(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 7, k, x)
#else
#define KRML_MAYBE_FOR7(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 8 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR8(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 8, k, x)
#else
#define KRML_MAYBE_FOR8(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 9 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR9(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 9, k, x)
#else
#define KRML_MAYBE_FOR9(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 10 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR10(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 10, k, x)
#else
#define KRML_MAYBE_FOR10(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 11 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR11(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 11, k, x)
#else
#define KRML_MAYBE_FOR11(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 12 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR12(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 12, k, x)
#else
#define KRML_MAYBE_FOR12(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 13 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR13(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 13, k, x)
#else
#define KRML_MAYBE_FOR13(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 14 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR14(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 14, k, x)
#else
#define KRML_MAYBE_FOR14(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 15 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR15(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 15, k, x)
#else
#define KRML_MAYBE_FOR15(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#if 16 <= KRML_UNROLL_MAX
#define KRML_MAYBE_FOR16(i, z, n, k, x) KRML_UNROLL_FOR(i, z, 16, k, x)
#else
#define KRML_MAYBE_FOR16(i, z, n, k, x) KRML_ACTUAL_FOR(i, z, n, k, x)
#endif

#ifndef KRML_HOST_IGNORE
#define KRML_HOST_IGNORE(x) (void)(x)
#endif

#endif  // CRYPTO_HACL_LIB_H_
