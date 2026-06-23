/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_ENGINE_CMD_H
#define RK_RECOVERY_ENGINE_CMD_H

#include <stdbool.h>
#include <stdio.h>

#include "recovery/update_source.h"

struct storage_mgr;

/* updateEngine CLI parse result: filled by engine_main.c parse_args,
 * consumed by the subcommand handlers below. */
struct engine_args {
    const char          *image_url;
    const char          *version_url;
    const char          *save_path;
    const char          *upgrade_spec;
    const char          *misc_func;
    const char          *custom_define;
    enum update_src_type src_type;
    bool                 is_update;
    bool                 is_reboot;
    int                  pipefd;
};

/* --- Top-level subcommand handlers (engine_cmd.c). Each returns 0 on
 *     success, -1 on failure. cmd_pipe is owned by main; handlers use it
 *     but never close it. --- */
int cmd_update(struct storage_mgr *mgr, const struct engine_args *args,
               FILE *cmd_pipe);
int cmd_misc(struct storage_mgr *mgr, const struct engine_args *args,
             FILE *cmd_pipe);
int cmd_misc_custom(struct storage_mgr *mgr, const char *custom_define);

/* --- A/B slot commands (cmd_slot.c) --- */
int cmd_slot_now(struct storage_mgr *mgr);
int cmd_slot_other(struct storage_mgr *mgr);
int cmd_slot_display(struct storage_mgr *mgr);

/* --- MISC partition commands (cmd_misc.c) --- */
int cmd_misc_write_recovery_bcb(struct storage_mgr *mgr,
                                const char *image_url,
                                const char *remaining_list);

#endif /* RK_RECOVERY_ENGINE_CMD_H */
