// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstring>

#if defined(__AVX512F__) && defined(__AVX512BW__)
#include <immintrin.h>
#define NX2_BYTES_EQUAL_AVX512 1
#elif defined(__AVX2__)
#include <immintrin.h>
#define NX2_BYTES_EQUAL_AVX2 1
#elif defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define NX2_BYTES_EQUAL_SSE2 1
#endif

#include "common/common_types.h"

namespace VideoCommon {

/// Whether [a, a + size) and [b, b + size) hold the same bytes. Inline, 256 bytes per step with
/// AVX-512/AVX2 or 128 bytes with SSE2 with an early exit: memcmp is an out-of-line ucrt call that
/// clang does not expand for 4 KiB pages (zortos293/Lichii#717 Nx2BufCache::BytesEqual, EDEN_NX2_PAGE_CMP).
[[nodiscard]] inline bool Nx2BytesEqual(const u8* a, const u8* b, std::size_t size) {
#if defined(NX2_BYTES_EQUAL_AVX512)
    std::size_t offset = 0;
    for (; offset + 256 <= size; offset += 256) {
        const auto load512 = [offset](const u8* base, std::size_t index) {
            return _mm512_loadu_si512(reinterpret_cast<const void*>(base + offset + index * 64));
        };
        __m512i acc = _mm512_xor_si512(load512(a, 0), load512(b, 0));
        acc = _mm512_or_si512(acc, _mm512_xor_si512(load512(a, 1), load512(b, 1)));
        acc = _mm512_or_si512(acc, _mm512_xor_si512(load512(a, 2), load512(b, 2)));
        acc = _mm512_or_si512(acc, _mm512_xor_si512(load512(a, 3), load512(b, 3)));
        if (_mm512_test_epi64_mask(acc, acc) != 0) {
            return false;
        }
    }
    return offset == size || std::memcmp(a + offset, b + offset, size - offset) == 0;
#elif defined(NX2_BYTES_EQUAL_AVX2)
    std::size_t offset = 0;
    for (; offset + 256 <= size; offset += 256) {
        const auto load256 = [offset](const u8* base, std::size_t index) {
            return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(base + offset + index * 32));
        };
        __m256i acc = _mm256_xor_si256(load256(a, 0), load256(b, 0));
        acc = _mm256_or_si256(acc, _mm256_xor_si256(load256(a, 1), load256(b, 1)));
        acc = _mm256_or_si256(acc, _mm256_xor_si256(load256(a, 2), load256(b, 2)));
        acc = _mm256_or_si256(acc, _mm256_xor_si256(load256(a, 3), load256(b, 3)));
        acc = _mm256_or_si256(acc, _mm256_xor_si256(load256(a, 4), load256(b, 4)));
        acc = _mm256_or_si256(acc, _mm256_xor_si256(load256(a, 5), load256(b, 5)));
        acc = _mm256_or_si256(acc, _mm256_xor_si256(load256(a, 6), load256(b, 6)));
        acc = _mm256_or_si256(acc, _mm256_xor_si256(load256(a, 7), load256(b, 7)));
        if (!_mm256_testz_si256(acc, acc)) {
            return false;
        }
    }
    return offset == size || std::memcmp(a + offset, b + offset, size - offset) == 0;
#elif defined(NX2_BYTES_EQUAL_SSE2)
    std::size_t offset = 0;
    for (; offset + 128 <= size; offset += 128) {
        const auto load = [offset](const u8* base, std::size_t index) {
            return _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + offset + index * 16));
        };
        __m128i acc = _mm_xor_si128(load(a, 0), load(b, 0));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 1), load(b, 1)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 2), load(b, 2)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 3), load(b, 3)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 4), load(b, 4)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 5), load(b, 5)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 6), load(b, 6)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 7), load(b, 7)));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(acc, _mm_setzero_si128())) != 0xFFFF) {
            return false;
        }
    }
    return offset == size || std::memcmp(a + offset, b + offset, size - offset) == 0;
#else
    return std::memcmp(a, b, size) == 0;
#endif
}

} // namespace VideoCommon
