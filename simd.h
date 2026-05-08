#ifndef SIMD_H
#define SIMD_H

#include <stddef.h>

/* AVX2 vectorized comma search. Returns number of commas found (up to max_pos). */
int simd_find_commas(const char *s, size_t len, int *out_positions, int max_pos);

/* AVX2 non-temporal memset for large aligned regions. */
void simd_memset(void *dst, int c, size_t n);

/* AVX2 streaming memcpy for large copies. */
void simd_memcpy(void *dst, const void *src, size_t n);

#endif
