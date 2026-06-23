/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "cmd:misc"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "recovery/bootmsg.h"
#include "recovery/storage.h"
#include "recovery/log.h"

/*
 * Write BCB for recovery-mode update.
 * When in non-A/B recovery mode, write a BCB message telling recovery
 * to update from the given image_url on next boot.
 *
 * remaining_list: CSV of base partition names still to flash after the
 *   recovery partition itself (e.g. "boot,rootfs,oem"), embedded in the
 *   BCB recovery args as --upgrade=. recovery parses it via get_args and
 *   forwards it to updateEngine on the next boot. NULL → "all".
 */
int cmd_misc_write_recovery_bcb(struct storage_mgr *mgr,
                                const char *image_url,
                                const char *remaining_list)
{
    struct bootloader_message msg;
    memset(&msg, 0, sizeof(msg));

    snprintf(msg.command, sizeof(msg.command), "boot-recovery");
    snprintf(msg.recovery, sizeof(msg.recovery),
             "recovery\n--update_package=%s\n--upgrade=%s\n",
             image_url, remaining_list ? remaining_list : "all");

    return bcb_write(mgr, &msg);
}
