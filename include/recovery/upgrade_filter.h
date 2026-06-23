/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_UPGRADE_FILTER_H
#define RK_RECOVERY_UPGRADE_FILTER_H

#include <stdbool.h>
#include "recovery/config.h"

struct upgrade_filter {
    bool all;                       /* --upgrade=all -> flash everything in image */
    char names[MAX_PARTS][32];      /* explicit base names / !name exclusions */
    int  count;
};

/* Parse a CSV of "all" and/or partition names into f. "all" (anywhere in
 * the list) sets the sweep flag; other tokens are explicit names, so
 * "all,parameter" means "everything + the GPT". "!name" or "-name" excludes
 * a partition from an all sweep. NULL/empty -> count=0, all=false (nothing
 * allowed; engine_main rejects that before upgrading). */
void upgrade_filter_parse(struct upgrade_filter *f, const char *spec);

/* True if `name` (A/B suffix stripped) is allowed by the filter.
 * NOTE: "all" deliberately does NOT include the high-risk pair parameter
 * (GPT) and bootloader (IDBlock) — those must be named explicitly. */
bool upgrade_allows(const struct upgrade_filter *f, const char *name);

#endif /* RK_RECOVERY_UPGRADE_FILTER_H */
