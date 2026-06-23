/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_LOG_H
#define RK_RECOVERY_LOG_H

#include <stdio.h>

enum log_level {
    LOG_ERR,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG,
};

void rk_log(enum log_level level, const char *module,
            const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Set global log level (default: LOG_INFO; recovery + updateEngine call
 * this with LOG_DEBUG at startup so console + log file get every line). */
void rk_log_set_level(enum log_level level);

/*
 * Replace stdout/stderr with a virtual FILE* that writes every printf /
 * fprintf to BOTH the original console fd AND `log_file` (opened for
 * append). Called unconditionally at startup by recovery and updateEngine
 * so live console output stays visible while the run is also persisted
 * for finish_recovery to copy off the rootfs.
 *
 * Synchronous (no subprocess), so the tail of the log can't be lost on
 * reboot. Safe to call once at startup; subsequent calls are no-ops in
 * the same process. Best-effort: if `log_file` cannot be opened the
 * console output still works.
 */
void rk_log_install_dual_writer(const char *log_file);

#ifndef __MODULE__
#define __MODULE__ "unknown"
#endif

#define pr_err(fmt, ...)    rk_log(LOG_ERR,   __MODULE__, fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   rk_log(LOG_WARN,  __MODULE__, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   rk_log(LOG_INFO,  __MODULE__, fmt, ##__VA_ARGS__)
#define pr_dbg(fmt, ...)    rk_log(LOG_DEBUG, __MODULE__, fmt, ##__VA_ARGS__)

/* ---- Unified error codes ---- */
enum rk_err {
    RK_OK            =  0,
    RK_ERR_IO        = -1,
    RK_ERR_NOMEM     = -2,
    RK_ERR_INVALID   = -3,
    RK_ERR_NOT_FOUND = -4,
    RK_ERR_VERIFY    = -5,
    RK_ERR_STORAGE   = -6,
    RK_ERR_MOUNT     = -7,
    RK_ERR_PATCH     = -8,
};

#endif /* RK_RECOVERY_LOG_H */
