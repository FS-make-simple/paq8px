#ifndef PAQ8PX_BUCKET_HPP
#define PAQ8PX_BUCKET_HPP

#include <cstdint>
#include <cstring>
#include "Shared.hpp"
#include "utils.hpp"

#if defined(__i386__) || defined(__x86_64__) || defined(_M_X64) // intel architecture
#include <immintrin.h>
#elif defined(__ARM_FEATURE_SIMD32) || defined(__ARM_NEON) // arm neon
#include <arm_neon.h>

// Below functions taken and adapted from https://github.com/jratcliff63367/sse2neon/blob/master/SSE2NEON.h
static inline int32x4_t _mm_srli_si128(int32x4_t a, int imm) {
  if ((imm) <= 0)
    return a;
  else if ((imm) > 15)
    return vdupq_n_s32(0);
  else
    return vreinterpretq_s32_s8(vextq_s8(vreinterpretq_s8_s32(a), vdupq_n_s8(0), (imm)));
}

static inline int _mm_movemask_epi8(int32x4_t _a)
{
  uint8x16_t input = vreinterpretq_u8_s32(_a);
  static const int8_t __attribute__((aligned(16))) xr[8] = { -7, -6, -5, -4, -3, -2, -1, 0 };
  uint8x8_t mask_and = vdup_n_u8(0x80);
  int8x8_t mask_shift = vld1_s8(xr);
  
  uint8x8_t lo = vget_low_u8(input);
  uint8x8_t hi = vget_high_u8(input);
  
  lo = vand_u8(lo, mask_and);
  lo = vshl_u8(lo, mask_shift);

  hi = vand_u8(hi, mask_and);
  hi = vshl_u8(hi, mask_shift);
  
  lo = vpadd_u8(lo, lo);
  lo = vpadd_u8(lo, lo);
  lo = vpadd_u8(lo, lo);

  hi = vpadd_u8(hi, hi);
  hi = vpadd_u8(hi, hi);
  hi = vpadd_u8(hi, hi);

  return ((hi[0] << 8) | (lo[0] & 0xFF));
}

// Below functions taken and adapted from https://github.com/DLTcollab/sse2neon/blob/master/sse2neon.h
static inline int32x4_t _mm_setr_epi8(signed char b0, signed char b1, signed char b2, signed char b3,
                                      signed char b4, signed char b5, signed char b6, signed char b7,
                                      signed char b8, signed char b9, signed char b10, signed char b11,
                                      signed char b12, signed char b13, signed char b14, signed char b15) {
  int8_t __attribute__((aligned(16))) data[16] = { (int8_t)b0,  (int8_t)b1,  (int8_t)b2,  (int8_t)b3,
                                                   (int8_t)b4,  (int8_t)b5,  (int8_t)b6,  (int8_t)b7,
                                                   (int8_t)b8,  (int8_t)b9,  (int8_t)b10, (int8_t)b11,
                                                   (int8_t)b12, (int8_t)b13, (int8_t)b14, (int8_t)b15 };
  return (int32x4_t)vld1q_s8(data);
}

static inline int32x4_t _mm_sad_epu8(int32x4_t a, int32x4_t b) {
  uint16x8_t t = vpaddlq_u8(vabdq_u8((uint8x16_t)a, (uint8x16_t)b));
  uint16_t r0 = t[0] + t[1] + t[2] + t[3];
  uint16_t r4 = t[4] + t[5] + t[6] + t[7];
  uint16x8_t r = vsetq_lane_u16(r0, vdupq_n_u16(0), 0);
  return (int32x4_t) vsetq_lane_u16(r4, r, 4);
}

static inline int32x4_t _mm_shuffle_epi8(int32x4_t a, int32x4_t b) {
  int8x16_t tbl = vreinterpretq_s8_s32(a);   // input a
  uint8x16_t idx = vreinterpretq_u8_s32(b);  // input b
  uint8x16_t idx_masked = vandq_u8(idx, vdupq_n_u8(0x8F));  // avoid using meaningless bits
#if defined(__aarch64__)
  return vreinterpretq_s32_s8(vqtbl1q_s8(tbl, idx_masked)); //function only exist on ARMv8
#elif defined(__GNUC__)
  int8x16_t ret;
  // %e and %f represent the even and odd D registers
  // respectively.
  __asm__(
      "    vtbl.8  %e[ret], {%e[tbl], %f[tbl]}, %e[idx]\n"
      "    vtbl.8  %f[ret], {%e[tbl], %f[tbl]}, %f[idx]\n"
      : [ret] "=&w" (ret)
      : [tbl] "w" (tbl), [idx] "w" (idx_masked));
  return vreinterpretq_s32_s8(ret);
#else
  // use this line if testing on aarch64
  int8x8x2_t a_split = { vget_low_s8(tbl), vget_high_s8(tbl) };
  return vreinterpretq_s32_s8(vcombine_s8(vtbl2_s8(a_split, vget_low_u8(idx_masked)), vtbl2_s8(a_split, vget_high_u8(idx_masked))));
#endif
}
#endif

static inline auto clz(uint32_t value) -> uint32_t {
  return __builtin_clz(value);
}

static inline auto ctz(uint32_t value) -> uint32_t {
  return __builtin_ctz(value);
}

/**
 * Hash bucket, 64 bytes
 *
 * Contains bit histories (statistics only) and byte histories (byte values).
 * For every byte the bit histories are stored in the "bitState" array
 * (7 bytes) in 3 different buckets:
 * The 1st bucket: 
 *  [bit statistics for bit 0] 
 *  [bit statistics for bit 1 when bit 0 was 0] 
 *  [bit statistics for bit 1 when bit 0 was 1] 
 *  [union of byte run count and the pending flag (0xFF)]
 *  [byte value: byte in context #1 - belongs to run count]
 *  [byte value: byte in context #2]
 *  [byte value: byte in context #3]
 * The 2nd bucket:
 *  [bit statistics for bit 2]
 *  [bit statistics for bit 3 when bit 2 was 0]
 *  [bit statistics for bit 3 when bit 2 was 1]
 *  [bit statistics for bit 4 when bit 2-3 was 00]
 *  [bit statistics for bit 4 when bit 2-3 was 01]
 *  [bit statistics for bit 4 when bit 2-3 was 10]
 *  [bit statistics for bit 4 when bit 2-3 was 11]
 * The 3rd bucket:
 *  [bit statistics for bit 5]
 *  [bit statistics for bit 6 when bit 5 was 0]
 *  [bit statistics for bit 6 when bit 5 was 1]
 *  [bit statistics for bit 7 when bit 5-6 was 00]
 *  [bit statistics for bit 7 when bit 5-6 was 01]
 *  [bit statistics for bit 7 when bit 5-6 was 10]
 *  [bit statistics for bit 7 when bit 5-6 was 11]

 */
class Bucket {
    uint16_t checksums[7]; /**< byte context checksums */
    uint8_t mostRecentlyUsed; /**< last 2 accesses (0-6) in low, high nibble */
public:
    uint8_t bitState[7][7]; /**< byte context, 3-bit context -> bit history state */
    // bitState[][0] = 1st bit, bitState[][1,2] = 2nd bit, bitState[][3..6] = 3rd bit
    // bitState[][0] is also a replacement priority, 0 = empty

    /**
     * Find or create hash element matching checksum.
     * If not found, insert or replace lowest priority (skipping 2 most recent).
     * @param checksum
     * @return
     */

#if (defined(__GNUC__) || defined(__clang__)) && (!defined(__ARM_FEATURE_SIMD32) && !defined(__ARM_NEON))
    __attribute__((target("ssse3")))
#endif
    inline auto findSsse3(const uint16_t checksum) -> uint8_t* {
#if !defined(__i386__) && !defined(__x86_64__) && !defined(_M_X64)
      return 0;
#else
      if (checksums[mostRecentlyUsed & 15U] == checksum) {
        return &bitState[mostRecentlyUsed & 15U][0];
      }
      const __m128i xmmChecksum = _mm_set1_epi16(short(checksum)); //fill 8 ch values
      // load 8 values, discard last one as only 7 are needed.
      // reverse order and compare 7 checksums values to @ref checksum
      // get mask is set get first index and return value
      __m128i tmp = _mm_load_si128(reinterpret_cast<__m128i *>(&checksums[0])); //load 8 values (8th will be discarded)

      tmp = _mm_shuffle_epi8(tmp, _mm_setr_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1));
      tmp = _mm_cmpeq_epi16(tmp, xmmChecksum); // compare ch values
      tmp = _mm_packs_epi16(tmp, _mm_setzero_si128()); // pack result
      uint32_t t = (_mm_movemask_epi8(tmp)) >> 1; // get mask of comparison, bit is set if eq, discard 8th bit
      uint32_t idx = 0; // index into bitState
      if( t != 0u ) {
        idx = (clz(t) - 1) & 7U;
        mostRecentlyUsed = static_cast<uint8_t>(mostRecentlyUsed << 4 | idx);
        return &bitState[idx][0];
      }
      __m128i lastL = _mm_set1_epi8((mostRecentlyUsed & 15U));
      __m128i lastH = _mm_set1_epi8((mostRecentlyUsed >> 4U));
      __m128i one1 = _mm_set1_epi8(1);
      __m128i vm = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7);
      
      __m128i lastX = _mm_unpacklo_epi64(lastL, lastH); // mostRecentlyUsed&15 mostRecentlyUsed>>4
      __m128i eq0 = _mm_cmpeq_epi8(lastX, vm); // compare values

      eq0 = _mm_or_si128(eq0, _mm_srli_si128(eq0, 8));    // or low values with high

      lastX = _mm_and_si128(one1, eq0);                //set to 1 if eq
      __m128i sum1 = _mm_sad_epu8(lastX, _mm_setzero_si128());        // count values, abs(a0 - b0) + abs(a1 - b1) .... up to b8
      const uint32_t pCount = _mm_cvtsi128_si32(sum1); // population count
      uint32_t t0 = (~_mm_movemask_epi8(eq0));
      int worst = 0xFFFF;
      for( int i = pCount; i < 7; ++i ) {
        int bitt = ctz(t0);     // get index
        t0 &= ~(1 << bitt); // clear bit set and test again
        int pri = bitState[bitt][0];
        if( pri < worst ) {
          worst = pri;
          idx = bitt;
        }
      }
      mostRecentlyUsed = 0xF0U | idx;
      checksums[idx] = checksum;
      return static_cast<uint8_t*>(memset(&bitState[idx][0], 0, 7));
#endif
    }

    inline auto findNeon(const uint16_t checksum) -> uint8_t* {
#if (!defined(__ARM_FEATURE_SIMD32) && !defined(__ARM_NEON))
      return 0;
#else
      if (checksums[mostRecentlyUsed & 15U] == checksum) {
          return &bitState[mostRecentlyUsed & 15U][0];
      }
      int worst = 0xFFFF;
      int idx = 0;
      const int32x4_t xmmChecksum = vreinterpretq_s32_s16(vdupq_n_s16(short(checksum))); //fill 8 ch values
      // load 8 values, discard last one as only 7 are needed.
      // reverse order and compare 7 checksums values to @ref checksum
      // get mask is set get first index and return value
      int32x4_t tmp = vld1q_s32(reinterpret_cast<int *>(&checksums[0])); //load 8 values (8th will be discarded)
      
      tmp = _mm_shuffle_epi8(tmp, _mm_setr_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1));
      tmp = vreinterpretq_s32_u16(vceqq_s16(vreinterpretq_s16_s32(tmp), vreinterpretq_s16_s32(xmmChecksum))); // compare ch values
      tmp = vreinterpretq_s32_s8(vcombine_s8(vqmovn_s16(vreinterpretq_s16_s32(tmp)), vqmovn_s16(vreinterpretq_s16_s32(vdupq_n_s32(0))))); // pack result
      uint32_t t = (_mm_movemask_epi8(tmp)) >> 1; // get mask of comparison, bit is set if eq, discard 8th bit
      uint32_t a = 0;    // index into bitState or 7 if not found
      if ( t != 0u ) {
        a = (clz(t) - 1) & 7U;
        mostRecentlyUsed = mostRecentlyUsed << 4U | a;
        return &bitState[a][0];
      }

      int32x4_t lastL = vreinterpretq_s32_s8(vdupq_n_s8((mostRecentlyUsed & 15U)));
      int32x4_t lastH = vreinterpretq_s32_s8(vdupq_n_s8((mostRecentlyUsed >> 4U)));
      int32x4_t one1 = vreinterpretq_s32_s8(vdupq_n_s8(1));
      int32x4_t vm = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7);

      int32x4_t lastX = vreinterpretq_s32_s64(vcombine_s64(vget_low_s64(vreinterpretq_s64_s32(lastL)), vget_low_s64(vreinterpretq_s64_s32(lastH)))); // mostRecentlyUsed&15 mostRecentlyUsed>>4
      int32x4_t eq0 = vreinterpretq_s32_u8(vceqq_s8(vreinterpretq_s8_s32(lastX), vreinterpretq_s8_s32(vm))); // compare values

      eq0 = vorrq_s32(eq0, _mm_srli_si128(eq0, 8));    // or low values with high

      lastX = vandq_s32(one1, eq0); //set to 1 if eq
      int32x4_t sum1 = _mm_sad_epu8(lastX, vdupq_n_s32(0)); // count values, abs(a0 - b0) + abs(a1 - b1) .... up to b8
      const uint32_t pCount = vgetq_lane_s32(sum1, 0); // population count
      uint32_t t0 = (~_mm_movemask_epi8(eq0));
      for( int i = pCount; i < 7; ++i ) {
        int bitt = ctz(t0);     // get index
        t0 &= ~(1 << bitt); // clear bit set and test again
        int pri = bitState[bitt][0];
        if( pri < worst ) {
          worst = pri;
          idx = bitt;
        }
      }
      mostRecentlyUsed = 0xF0U | idx;
      checksums[idx] = checksum;
      return static_cast<uint8_t*>(memset(&bitState[idx][0], 0, 7));
#endif
    }

    inline auto findNone(const uint16_t checksum) -> uint8_t* {
      if( checksums[mostRecentlyUsed & 15U] == checksum ) {
        return &bitState[mostRecentlyUsed & 15U][0];
      }
      int worst = 0xFFFF, idx = 0;
      for( int i = 0; i < 7; ++i ) {
        if( checksums[i] == checksum ) {
          mostRecentlyUsed = mostRecentlyUsed << 4U | i;
          return &bitState[i][0];
        }
        if( bitState[i][0] < worst && (mostRecentlyUsed & 15U) != i && mostRecentlyUsed >> 4U != i ) {
          worst = bitState[i][0];
          idx = i;
        }
      }
      mostRecentlyUsed = 0xF0U | idx;
      checksums[idx] = checksum;
      return (uint8_t *) memset(&bitState[idx][0], 0, 7);
    }

    inline auto find(const uint16_t checksum, const SIMD chosenSimd) {
#if defined(__i386__) || !defined(__x86_64__) || defined(_M_X64)
      if (chosenSimd == SIMD_AVX2 || chosenSimd == SIMD_SSSE3) {
        return findSsse3(checksum);
      }
#endif
#if (defined(__ARM_FEATURE_SIMD32) && defined(__ARM_NEON))
      if ( chosenSimd == SIMD_NEON ) {
        return findNeon(checksum);
      }
#endif
      return findNone(checksum);
    }
};

#endif //PAQ8PX_BUCKET_HPP
