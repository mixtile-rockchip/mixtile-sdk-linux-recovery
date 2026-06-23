/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "recovery/cmdline.h"

#define CMDLINE_PATH    "/proc/cmdline"
#define CMDLINE_LENGTH  4096

int cmdline_read(char *buf, size_t len)
{
    if (!buf || len < 2)
        return -1;
    int fd = open(CMDLINE_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return n;
}

bool cmdline_contains(const char *keyword)
{
    if (!keyword)
        return false;
    char buf[CMDLINE_LENGTH];
    if (cmdline_read(buf, sizeof(buf)) < 0)
        return false;
    return strstr(buf, keyword) != NULL;
}

int cmdline_get_value(const char *key, char *out, size_t len)
{
    if (!key || !out || len < 2)
        return -1;
    char buf[CMDLINE_LENGTH];
    if (cmdline_read(buf, sizeof(buf)) < 0)
        return -1;

    const char *p = strstr(buf, key);
    if (!p)
        return -1;
    p = strchr(p, '=');
    if (!p)
        return -1;
    p++;
    while (*p == ' ')
        p++;

    size_t i = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r' && i < len - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0 ? 0 : -1;
}
