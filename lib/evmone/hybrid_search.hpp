// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2021 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#if _MSC_VER
#define ALWAYS_INLINE __forceinline
#elif __has_attribute(always_inline)
#define EVMONE_ALWAYS_INLINE __attribute__((always_inline))
#else
#define EVMONE_ALWAYS_INLINE
#endif

namespace evmone
{
namespace detail
{
inline EVMONE_ALWAYS_INLINE const int32_t* hybrid_search_implementation(
    const int32_t* vec, size_t n, int32_t key)
{
    if (n == 0)
        return vec;

    if (n > 128 || n % 8 != 0)
        return std::lower_bound(vec, vec + n, key);

    int pos = 0;
    for (size_t i = 0; i < n;)
    {
        pos += (vec[i++] < key);
        pos += (vec[i++] < key);
        pos += (vec[i++] < key);
        pos += (vec[i++] < key);
        pos += (vec[i++] < key);
        pos += (vec[i++] < key);
        pos += (vec[i++] < key);
        pos += (vec[i++] < key);
    }
    return vec + pos;
}
}  // namespace detail

#if defined(__x86_64__) && __has_attribute(target)

__attribute__((target("default"))) inline const int32_t* hybrid_search(
    const int32_t* vec, size_t n, int32_t key)
{
    return detail::hybrid_search_implementation(vec, n, key);
}

__attribute__((target("avx2"))) inline const int32_t* hybrid_search(
    const int32_t* vec, size_t n, int32_t key)
{
    return detail::hybrid_search_implementation(vec, n, key);
}

#else

inline const int32_t* hybrid_search(const int32_t* vec, size_t n, int32_t key)
{
    return detail::hybrid_search_implementation(vec, n, key);
}

#endif

}  // namespace evmone
