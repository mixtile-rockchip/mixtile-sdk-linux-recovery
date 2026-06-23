/* SPDX-License-Identifier: Apache-2.0 */
/*
 * strlcat — safe string concatenation.
 * Appends at most dst_size - strlen(dst) - 1 characters, always NUL-terminates.
 * Returns strlen(src) + min(dst_size, strlen(dst)).
 */

#include <stddef.h>
#include <string.h>

size_t strlcat(char *dst, const char *src, size_t dst_size)
{
    size_t dst_len = strlen(dst);
    size_t src_len = strlen(src);

    if (dst_len >= dst_size)
        return dst_size + src_len;

    size_t remain = dst_size - dst_len - 1;
    if (src_len < remain) {
        memcpy(dst + dst_len, src, src_len + 1);
    } else {
        memcpy(dst + dst_len, src, remain);
        dst[dst_size - 1] = '\0';
    }

    return dst_len + src_len;
}
