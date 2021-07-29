// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2021 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace evmone
{
typedef const int32_t* (*hybrid_search_func)(const int32_t* vec, size_t n, int32_t key);

extern hybrid_search_func hybrid_search;

}  // namespace evmone
