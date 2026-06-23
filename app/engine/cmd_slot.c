/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "cmd:slot"

#include <stdio.h>

#include "recovery/bootmsg.h"
#include "recovery/storage.h"
#include "recovery/log.h"

int cmd_slot_now(struct storage_mgr *mgr)
{
    pr_info("--misc=now: marking current slot as successful\n");
    return set_slot_succeed(mgr);
}

int cmd_slot_other(struct storage_mgr *mgr)
{
    int current = get_current_slot();
    pr_info("--misc=other: switching from slot %d\n", current);
    return set_slot_activity(mgr, current);
}

int cmd_slot_display(struct storage_mgr *mgr)
{
    misc_display(mgr);
    return 0;
}
