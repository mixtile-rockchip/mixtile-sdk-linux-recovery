/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_RECOVERY_LOCAL_H
#define RK_RECOVERY_RECOVERY_LOCAL_H

#include <stdbool.h>
#include "recovery/update_source.h"

struct storage_mgr;

/* --- recovery CLI parse result: filled by recovery_main parse_args,
 *     consumed by the dispatch handlers below. --- */
struct recovery_args {
    const char *send_intent;
    const char *update_package;
    const char *upgrade_list;
    bool        wipe;
    bool        wipe_all;
    bool        show_text;
};

/* --- BCB / command-file arg parsing --- */
void get_args(int *argc, char ***argv, struct storage_mgr *mgr);

/* --- End-of-run cleanup: flush log, write intent, clear BCB --- */
void finish_recovery(struct storage_mgr *mgr, const char *send_intent);

/* --- Spawn updateEngine and forward its progress pipe ---
 * upgrade_list: --upgrade= value for OTA (SRC_LOCAL), required & non-NULL;
 * ignored for SD/USB (those force --upgrade=all internally). */
int do_rk_updateEngine(const char *image_path,
                       enum update_src_type src_type,
                       const char *upgrade_list);

/* --- Top-level dispatch handlers (recovery_cmd.c). Each returns 0 on
 *     success, non-zero on failure (forwarded to main's status). --- */
int run_ota_update(const struct recovery_args *args);
int run_media_update(struct storage_mgr *mgr, const char *send_intent,
                     enum update_src_type src, const char *image_path,
                     const char *mount_point, const char *media_label);

/* --- Volume wipe helpers --- */
int erase_volume(const char *volume);
int wipe_data(void);

/*
 * Show AOSP-style error menu and block until the user picks an action.
 * Loops on actions that should not reboot (wipe, retry apply); returns
 * once the user picks reboot or an apply action succeeds. Always returns
 * ITEM_REBOOT so the caller drops into its reboot path.
 *
 * Only available when built with -DRK_UI_ENABLE (RecoveryNoUi=0).
 * The UI must be initialized; finish_recovery() should have cleared the
 * BCB *before* calling this so a user-picked reboot doesn't loop back
 * into recovery.
 */
int prompt_and_wait(void);

#endif /* RK_RECOVERY_RECOVERY_LOCAL_H */
