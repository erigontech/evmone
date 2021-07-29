#include "hybrid_search.hpp"

#include <algorithm>

#if _MSC_VER
#define ALWAYS_INLINE __forceinline
#elif __has_attribute(always_inline)
#define ALWAYS_INLINE __attribute__((always_inline))
#else
#define ALWAYS_INLINE
#endif

namespace evmone
{
static inline ALWAYS_INLINE const int32_t* hybrid_search_implementation(
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

static const int32_t* hybrid_search_generic(const int32_t* vec, size_t n, int32_t key)
{
    return hybrid_search_implementation(vec, n, key);
}

hybrid_search_func hybrid_search = hybrid_search_generic;


#if defined(__x86_64__) && __has_attribute(target)
__attribute__((target("avx2"))) static const int32_t* hybrid_search_avx2(
    const int32_t* vec, size_t n, int32_t key)
{
    return hybrid_search_implementation(vec, n, key);
}

__attribute__((constructor)) static void select_hybrid_search_implementation()
{
    __builtin_cpu_init();

    if (__builtin_cpu_supports("avx2"))
        hybrid_search = hybrid_search_avx2;
}
#endif

}  // namespace evmone