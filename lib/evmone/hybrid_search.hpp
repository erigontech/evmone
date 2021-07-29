// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2021 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>

namespace evmone
{
template <typename T, size_t N>
inline const T* linear_search(const T* vec, const T& key)
{
    int cnt = 0;
    for (size_t i = 0; i < N; ++i)
    {
        cnt += (vec[i] < key);
    }
    return vec + cnt;
}

template <typename T>
inline const T* hybrid_search(const T* vec, size_t n, const T& key)
{
    if (n == 0)
        return vec;

    int pos = 0;
    int step = -1;

#if defined(__GNUC__) && !defined(__clang__)
    // For some reason gcc doesn't generate cmov with the ternary operator
#define EVMONE_BINARY_SEARCH_BRANCHLESS_STEP pos += (vec[pos + step] <= key) * step;
#else
#define EVMONE_BINARY_SEARCH_BRANCHLESS_STEP pos = (vec[pos + step] <= key ? pos + step : pos)
#endif

    switch (n)
    {
    case 1u << 10:
        step = 1 << 9;
        EVMONE_BINARY_SEARCH_BRANCHLESS_STEP;
        [[fallthrough]];
    case 1u << 9:
        step = 1 << 8;
        EVMONE_BINARY_SEARCH_BRANCHLESS_STEP;
        [[fallthrough]];
    case 1u << 8:
        step = 1 << 7;
        EVMONE_BINARY_SEARCH_BRANCHLESS_STEP;
        [[fallthrough]];
    case 1u << 7:
        step = 1 << 6;
        EVMONE_BINARY_SEARCH_BRANCHLESS_STEP;
        [[fallthrough]];
    case 1u << 6:
        step = 1 << 5;
        EVMONE_BINARY_SEARCH_BRANCHLESS_STEP;
        [[fallthrough]];
    case 1u << 5:
        step = 1 << 4;
        EVMONE_BINARY_SEARCH_BRANCHLESS_STEP;
        [[fallthrough]];
    case 1u << 4:
        return linear_search<T, 1u << 4>(vec + pos, key);
    case 1u << 3:
        return linear_search<T, 1u << 3>(vec + pos, key);
    case 1u << 2:
        return linear_search<T, 1u << 2>(vec + pos, key);
    case 1u << 1:
        return linear_search<T, 1u << 1>(vec + pos, key);
    case 1u << 0:
        return linear_search<T, 1u << 0>(vec + pos, key);
    }

#undef EVMONE_BINARY_SEARCH_BRANCHLESS_STEP

    return std::lower_bound(vec, vec + n, key);
}

}  // namespace evmone
