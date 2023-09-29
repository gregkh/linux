/* 
 * Copyright (c) 2016-2022 INRIA, CMU and Microsoft Corporation
 * Copyright (c) 2022-2023 HACL* Contributors
 * Copyright (c) 2023 Cryspen
 */

#ifndef __Hacl_LIB_H
#define __Hacl_LIB_H

#include <asm/unaligned.h>
#include <crypto/sha256_base.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

typedef u128 FStar_UInt128_uint128;

static inline u128 FStar_UInt128_shift_left(u128 x, u32 y) {
  return (x << y);
}

static inline u128 FStar_UInt128_add(u128 x, u128 y) {
  return (x + y);
}

static inline u128 FStar_UInt128_uint64_to_uint128(u64 x) {
  return ((u128)x);
}


/* Loads and stores. These avoid undefined behavior due to unaligned memory
 * accesses, via memcpy. */

#define load32_be(b) (get_unaligned_be32(b))
#define store32_be(b, i) put_unaligned_be32(i, b);
#define load64_be(b) (get_unaligned_be64(b))
#define store64_be(b, i) put_unaligned_be64(i, b);

#define load32_le(b) (get_unaligned_le32(b))
#define store32_le(b, i) put_unaligned_le32(i, b);
#define load64_le(b) (get_unaligned_le64(b))
#define store64_le(b, i) put_unaligned_le64(i, b);

static inline void store128_be(u8* buf, u128 x) {
  store64_be(buf,(u64)(x>>64));
  store64_be(buf+8,(u64)(x));
}



/* Macros for prettier unrolling of loops */
#define KRML_LOOP1(i, n, x) { \
  x \
  i += n; \
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
  KRML_LOOP8(i, n, x) \
  KRML_LOOP2(i, n, x)

#define KRML_LOOP11(i, n, x) \
  KRML_LOOP8(i, n, x) \
  KRML_LOOP3(i, n, x)

#define KRML_LOOP12(i, n, x) \
  KRML_LOOP8(i, n, x) \
  KRML_LOOP4(i, n, x)

#define KRML_LOOP13(i, n, x) \
  KRML_LOOP8(i, n, x) \
  KRML_LOOP5(i, n, x)

#define KRML_LOOP14(i, n, x) \
  KRML_LOOP8(i, n, x) \
  KRML_LOOP6(i, n, x)

#define KRML_LOOP15(i, n, x) \
  KRML_LOOP8(i, n, x) \
  KRML_LOOP7(i, n, x)

#define KRML_LOOP16(i, n, x) \
  KRML_LOOP8(i, n, x) \
  KRML_LOOP8(i, n, x)

#define KRML_UNROLL_FOR(i, z, n, k, x) do { \
  uint32_t i = z; \
  KRML_LOOP##n(i, k, x) \
} while (0)

#define KRML_ACTUAL_FOR(i, z, n, k, x) \
  do { \
    for (uint32_t i = z; i < n; i += k) { \
      x \
    } \
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

/* For "bare" targets that do not have a C stdlib, the user might want to use
 * [-add-early-include '"mydefinitions.h"'] and override these. */
#ifndef KRML_HOST_PRINTF
#  define KRML_HOST_PRINTF(...)
#endif
#ifndef KRML_HOST_EPRINTF
#  define KRML_HOST_EPRINTF(...)
#endif

#ifndef KRML_HOST_EXIT
#  define KRML_HOST_EXIT exit
#endif

#ifndef KRML_HOST_IGNORE
#  define KRML_HOST_IGNORE(x) (void)(x)
#endif


#endif
