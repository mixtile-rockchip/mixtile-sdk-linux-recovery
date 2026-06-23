/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "upgrade_filter"

#include <stdio.h>
#include <string.h>

#include "recovery/upgrade_filter.h"

/* Copy `in` to `out` with a trailing _a / _b A/B suffix stripped. */
static void base_name(const char *in, char *out, size_t n)
{
    snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    if (l >= 2 && out[l - 2] == '_' && (out[l - 1] == 'a' || out[l - 1] == 'b'))
        out[l - 2] = '\0';
}

static const char *filter_token_name(const char *token, bool *exclude)
{
    *exclude = (token[0] == '!' || token[0] == '-');
    return *exclude ? token + 1 : token;
}

/*
 * Partitions excluded from "all": GPT (parameter) and bootloader (IDBlock)
 * are risky and rarely wanted, and on MTD they can only be flashed when
 * explicitly exposed as named partitions in mtdparts/parameter. "all" never
 * sweeps them in — they must be named explicitly (e.g. --upgrade=parameter
 * or --upgrade=all,bootloader).
 */
static bool is_explicit_only(const char *base)
{
    return strcmp(base, "parameter") == 0 || strcmp(base, "bootloader") == 0;
}

void upgrade_filter_parse(struct upgrade_filter *f, const char *spec)
{
    f->all = false;
    f->count = 0;

    if (!spec || !spec[0])
        return;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);

    /* "all" may appear alone or mixed with explicit names (e.g.
     * "all,parameter" = everything + the GPT). A token of "all" sets the
     * sweep flag; other tokens are collected as explicit names. A token
     * prefixed with ! or - excludes a partition from an all sweep; this is
     * used by the normal-system two-step path to pass "all,!recovery" to the
     * newly flashed recovery. */
    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    while (tok && f->count < MAX_PARTS) {
        if (strcmp(tok, "all") == 0) {
            f->all = true;
        } else {
            snprintf(f->names[f->count], sizeof(f->names[0]), "%s", tok);
            f->count++;
        }
        tok = strtok_r(NULL, ",", &save);
    }
}

bool upgrade_allows(const struct upgrade_filter *f, const char *name)
{
    char nb[32];
    base_name(name, nb, sizeof(nb));

    /* Exclusions win over both explicit names and the all sweep. */
    for (int i = 0; i < f->count; i++) {
        bool exclude;
        const char *token = filter_token_name(f->names[i], &exclude);
        if (!exclude)
            continue;

        char eb[32];
        base_name(token, eb, sizeof(eb));
        if (strcmp(nb, eb) == 0)
            return false;
    }

    /* Explicit name match always allows — including the explicit-only pair. */
    for (int i = 0; i < f->count; i++) {
        bool exclude;
        const char *token = filter_token_name(f->names[i], &exclude);
        if (exclude)
            continue;

        char eb[32];
        base_name(token, eb, sizeof(eb));
        if (strcmp(nb, eb) == 0)
            return true;
    }

    /* "all" sweeps everything except the high-risk explicit-only partitions. */
    if (f->all && !is_explicit_only(nb))
        return true;

    return false;
}
