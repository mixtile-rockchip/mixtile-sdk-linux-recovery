/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_CMDLINE_H
#define RK_RECOVERY_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

/* ---- /proc/cmdline helpers ---- */

/*
 * Read /proc/cmdline into buf (NUL-terminated, truncated to len-1 bytes).
 * Returns bytes read (>0) on success, -1 on error.
 */
int  cmdline_read(char *buf, size_t len);

/*
 * Check whether /proc/cmdline contains a given substring (keyword).
 */
bool cmdline_contains(const char *keyword);

/*
 * Extract the value of "key=..." from /proc/cmdline into out.
 * Writes at most len-1 bytes plus a NUL. Leading spaces after '=' are skipped.
 * Returns 0 on success, -1 if key is absent or value is empty.
 */
int  cmdline_get_value(const char *key, char *out, size_t len);

#endif /* RK_RECOVERY_CMDLINE_H */
