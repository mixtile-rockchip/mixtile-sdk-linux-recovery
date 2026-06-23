/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "upgrade:version"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "recovery/upgrade_engine.h"
#include "recovery/image.h"
#include "recovery/download.h"
#include "recovery/log.h"

/*
 * Parse version string from a file.
 * Looks for "RK_VERSION=x.x.x" line. Returns true on success.
 * Ported from update_engine/rktools.c:getVersionFromfile().
 */
static bool get_version_from_file(const char *filepath, char *version,
                                  int max_len)
{
    if (!filepath || !version)
        return false;

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        pr_dbg("open %s failed: %s\n", filepath, strerror(errno));
        return false;
    }

    char *line = NULL;
    size_t len = 0;
    bool found = false;

    while (getline(&line, &len, fp) != -1) {
        if (line[0] == '#' || line[0] == '\n')
            continue;

        char *p = strstr(line, "RK_VERSION");
        if (p && (p = strchr(p, '=')) != NULL) {
            p++;
            while (*p == ' ')
                p++;
            /* Strip trailing newline/whitespace */
            int plen = (int)strlen(p);
            while (plen > 0 && (p[plen - 1] == '\n' || p[plen - 1] == '\r'
                                || p[plen - 1] == ' '))
                plen--;
            if (plen > max_len - 1)
                plen = max_len - 1;
            memcpy(version, p, plen);
            version[plen] = '\0';
            pr_dbg("parsed version: %s\n", version);
            found = true;
            break;
        }
    }

    free(line);
    fclose(fp);
    return found;
}

#define LOCAL_VERSION_PATH  "/etc/version"

void upgrade_get_sw_version(char *buffer, int max_len)
{
    if (get_version_from_file(LOCAL_VERSION_PATH, buffer, max_len))
        return;
    snprintf(buffer, max_len, "unknown");
}

/*
 * Semantic version comparison: compare "a.b.c" vs "x.y.z" numerically.
 * Returns >0 if a > b, <0 if a < b, 0 if equal.
 */
static int version_compare(const char *a, const char *b)
{
    const char *pa = a, *pb = b;

    while (*pa || *pb) {
        long va = 0, vb = 0;

        if (*pa) {
            char *ea;
            va = strtol(pa, &ea, 10);
            pa = (*ea == '.') ? ea + 1 : ea;
        }
        if (*pb) {
            char *eb;
            vb = strtol(pb, &eb, 10);
            pb = (*eb == '.') ? eb + 1 : eb;
        }

        if (va != vb)
            return (va > vb) ? 1 : -1;
    }
    return 0;
}

bool upgrade_check_version(const char *url)
{
    char source_ver[20] = {0};
    char target_ver[20] = {0};

    upgrade_get_sw_version(source_ver, sizeof(source_ver));

#if defined(RK_SOURCE_URL)
    /* If url is a remote version file, download and parse it */
    if (strncmp(url, "http", 4) == 0) {
        const char *tmp_ver = "/tmp/version";
        if (download_file(url, tmp_ver) != 0) {
            pr_err("download version file failed: %s\n", url);
            return false;
        }
        if (!get_version_from_file(tmp_ver, target_ver, sizeof(target_ver)))
            return false;
    } else
#endif
    {
        if (!image_get_version(url, target_ver, sizeof(target_ver)))
            return false;
    }

    pr_info("version check: target=%s current=%s\n", target_ver, source_ver);
    return (version_compare(target_ver, source_ver) > 0);
}
