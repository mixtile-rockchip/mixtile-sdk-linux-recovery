/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_UPGRADE_ENGINE_H
#define RK_RECOVERY_UPGRADE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "recovery/upgrade_filter.h"
#include "recovery/storage.h"
#include "recovery/update_source.h"

/* ---- Upgrade status ---- */
enum rk_upgrade_status {
    RK_UPGRADE_FINISHED,
    RK_UPGRADE_START,
    RK_UPGRADE_ERR,
};

/* ---- Callbacks ---- */
typedef void (*upgrade_status_fn)(void *user_data, enum rk_upgrade_status status);
typedef void (*upgrade_print_fn)(char *prompt, void *user_data);

/* ---- Upgrade context (replaces global variables) ---- */
struct upgrade_context {
    /* Input */
    char                image_url[512];
    char                save_path[256];
    enum update_src_type source_type;
    struct upgrade_filter upgrade;      /* --upgrade= whitelist (all | name,name,...) */

    /* Runtime */
    /* Staged local image: set by upgrade_run after download_file() to the
     * effective on-disk path (= save_path, else image_url). parse / diff /
     * flash / verify ALL read this — never image_url, which may be a remote
     * URL or a source that's gone after a two-step reboot. */
    char                image_path[512];
    struct storage_mgr  *storage;
    int                 current_slot;   /* -1 = non-AB */
    int                 pipe_fd;        /* IPC to recovery UI */

    /* Engine state (was module-global in engine_main.c) */
    enum rk_upgrade_status status;       /* updated by status_cb */
    FILE                   *cmd_pipe;    /* recovery UI pipe; NULL = none */

    /* Callbacks. cb_ctx is passed to status_cb and print_cb; defaults
     * to the ctx itself in upgrade_ctx_init so callbacks can reach back
     * into the context without a separate handle. */
    upgrade_status_fn   status_cb;
    upgrade_print_fn    print_cb;
    void                *cb_ctx;
};

/* ---- Public API ---- */

/* Initialize upgrade context with defaults */
void upgrade_ctx_init(struct upgrade_context *ctx);

/* Run the full upgrade pipeline (discover → plan → execute) */
int upgrade_run(struct upgrade_context *ctx);

/* Get software version from image */
void upgrade_get_sw_version(char *buffer, int max_len);

/* Check remote version */
bool upgrade_check_version(const char *url);

#endif /* RK_RECOVERY_UPGRADE_ENGINE_H */
