/* SPDX-License-Identifier: Apache-2.0 */
/*
 * strlcpy — safe string copy.
 * Copies at most dst_size-1 characters, always NUL-terminates.
 * Returns strlen(src).
 */

#include <stddef.h>

size_t strlcpy(char *dst, const char *src, size_t dst_size)
{
    const char *s = src;
    size_t n = dst_size;

    if (n != 0) {
        while (--n != 0) {
            if ((*dst++ = *s++) == '\0')
                return (size_t)(s - src - 1);
        }
        *dst = '\0';
    }

    while (*s++)
        ;
    return (size_t)(s - src - 1);
}
