/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2024 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "log"

/* fopencookie + cookie_io_functions_t (dual-output log writer) */
#define _GNU_SOURCE

#include "recovery/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>

static enum log_level g_log_level = LOG_INFO;

static const char *level_tag[] = {
    [LOG_ERR]   = "E",
    [LOG_WARN]  = "W",
    [LOG_INFO]  = "I",
    [LOG_DEBUG] = "D",
};

void rk_log_set_level(enum log_level level)
{
    g_log_level = level;
}

void rk_log(enum log_level level, const char *module,
            const char *fmt, ...)
{
    if (level > g_log_level)
        return;

    va_list ap;
    fprintf(stderr, "[%s][%s] ", level_tag[level], module);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- Dual-output log writer ----
 *
 * Install a virtual FILE* (via fopencookie) whose write callback fans
 * every printf/fprintf out to two real fds: the original console (for
 * live serial debug) and a log file (for post-mortem persistence).
 *
 * "Cookie" is the GNU-libc term for the private context handed back to
 * each callback — we use it to carry the two destination fds.
 *
 * Fully synchronous; no subprocess. Replaces stdout and stderr.
 */
struct dual_log_cookie {
    int console_fd;
    int file_fd;
};

static ssize_t dual_log_write(void *c, const char *buf, size_t len)
{
    struct dual_log_cookie *dc = c;
    /* Best-effort writes; console first so a slow filesystem doesn't
     * delay what the user sees. Short writes are tolerated for log
     * output — losing a byte beats blocking the upgrade. */
    if (dc->console_fd >= 0)
        (void)write(dc->console_fd, buf, len);
    if (dc->file_fd >= 0)
        (void)write(dc->file_fd, buf, len);
    return (ssize_t)len;
}

void rk_log_install_dual_writer(const char *log_file)
{
    /* Singleton: cookie and FILE must outlive every caller; second call
     * is a no-op. */
    static int installed = 0;
    static struct dual_log_cookie cookie = { .console_fd = -1, .file_fd = -1 };

    if (installed)
        return;

    cookie.console_fd = dup(STDERR_FILENO);
    if (cookie.console_fd < 0)
        pr_warn("dup console fd: %s\n", strerror(errno));

    if (log_file && log_file[0]) {
        cookie.file_fd = open(log_file,
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (cookie.file_fd < 0)
            pr_warn("open %s: %s — log will be console-only\n",
                    log_file, strerror(errno));
    }

    if (cookie.console_fd < 0 && cookie.file_fd < 0)
        return;     /* nothing to install; keep stock stdout/stderr */

    cookie_io_functions_t io = {
        .read  = NULL,
        .write = dual_log_write,
        .seek  = NULL,
        .close = NULL,      /* never closes — process lifetime */
    };
    FILE *dual = fopencookie(&cookie, "w", io);
    if (!dual) {
        pr_warn("fopencookie failed: %s\n", strerror(errno));
        return;
    }
    setvbuf(dual, NULL, _IONBF, 0);     /* unbuffered like the freopen path */

    stdout = dual;
    stderr = dual;
    installed = 1;
}
