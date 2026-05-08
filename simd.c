#include "simd.h"
#include <immintrin.h>
#include <stdint.h>
#include <string.h>

int simd_find_commas(const char *s, size_t len, int *out_positions, int max_pos)
{
    const __m256i comma = _mm256_set1_epi8(',');
    int count = 0;
    size_t i = 0;

    /* Process 32 bytes per iteration with AVX2 */
    for (; i + 32 <= len && count < max_pos; i += 32) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i eq = _mm256_cmpeq_epi8(chunk, comma);
        unsigned int mask = (unsigned int)_mm256_movemask_epi8(eq);
        while (mask && count < max_pos) {
            int bit = __builtin_ctz(mask);
            out_positions[count++] = (int)(i + (unsigned)bit);
            mask &= mask - 1;
        }
    }

    /* Scalar tail */
    for (; i < len && count < max_pos; i++) {
        if (s[i] == ',')
            out_positions[count++] = (int)i;
    }
    return count;
}

void simd_memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    __m256i v = _mm256_set1_epi8((char)c);

    /* Unaligned head to reach 32-byte alignment */
    while (n > 0 && ((uintptr_t)d & 31)) {
        *d++ = (unsigned char)c;
        n--;
    }

    /* Bulk non-temporal stores: 256 bytes per iteration */
    while (n >= 256) {
        _mm256_stream_si256((__m256i *)(d + 0),  v);
        _mm256_stream_si256((__m256i *)(d + 32), v);
        _mm256_stream_si256((__m256i *)(d + 64), v);
        _mm256_stream_si256((__m256i *)(d + 96), v);
        _mm256_stream_si256((__m256i *)(d + 128), v);
        _mm256_stream_si256((__m256i *)(d + 160), v);
        _mm256_stream_si256((__m256i *)(d + 192), v);
        _mm256_stream_si256((__m256i *)(d + 224), v);
        d += 256;
        n -= 256;
    }

    while (n >= 32) {
        _mm256_stream_si256((__m256i *)d, v);
        d += 32;
        n -= 32;
    }

    /* Scalar tail */
    while (n--)
        *d++ = (unsigned char)c;

    _mm_sfence();
}

void simd_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (n < 256) {
        memcpy(dst, src, n);
        return;
    }

    /* Unaligned head */
    while (n > 0 && ((uintptr_t)d & 31)) {
        *d++ = *s++;
        n--;
    }

    /* Bulk streaming copy: 256 bytes per iteration */
    while (n >= 256) {
        _mm256_stream_si256((__m256i *)(d + 0),  _mm256_loadu_si256((const __m256i *)(s + 0)));
        _mm256_stream_si256((__m256i *)(d + 32), _mm256_loadu_si256((const __m256i *)(s + 32)));
        _mm256_stream_si256((__m256i *)(d + 64), _mm256_loadu_si256((const __m256i *)(s + 64)));
        _mm256_stream_si256((__m256i *)(d + 96), _mm256_loadu_si256((const __m256i *)(s + 96)));
        _mm256_stream_si256((__m256i *)(d + 128), _mm256_loadu_si256((const __m256i *)(s + 128)));
        _mm256_stream_si256((__m256i *)(d + 160), _mm256_loadu_si256((const __m256i *)(s + 160)));
        _mm256_stream_si256((__m256i *)(d + 192), _mm256_loadu_si256((const __m256i *)(s + 192)));
        _mm256_stream_si256((__m256i *)(d + 224), _mm256_loadu_si256((const __m256i *)(s + 224)));
        d += 256;
        s += 256;
        n -= 256;
    }

    while (n >= 32) {
        _mm256_stream_si256((__m256i *)d, _mm256_loadu_si256((const __m256i *)s));
        d += 32;
        s += 32;
        n -= 32;
    }

    /* Scalar tail */
    while (n--)
        *d++ = *s++;

    _mm_sfence();
}
