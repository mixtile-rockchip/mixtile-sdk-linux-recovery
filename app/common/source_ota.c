/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "source:ota"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#include "recovery/update_source.h"
#include "recovery/config.h"
#include "recovery/download.h"
#include "recovery/log.h"

/*
 * Resolve image URL: download if remote, use directly if local.
 * On success, resolved_path is set to the local file path.
 * Returns 0 on success, -1 on error, -2 if local (no download needed).
 */
int source_ota_resolve(const char *url, const char *save_path,
                       char *resolved_path, size_t len)
{
#if !defined(RK_SOURCE_URL)
    (void)save_path;
#endif
    if (!url || !url[0]) {
        pr_err("OTA URL is empty\n");
        return -1;
    }

#if defined(RK_SOURCE_URL)
    if (strncmp(url, "http", 4) == 0) {
        /* Remote URL: download first */
        const char *dest = save_path && save_path[0] ? save_path : "/tmp/update.img";
        pr_info("downloading %s -> %s\n", url, dest);

        int ret = download_file(url, dest);
        if (ret != 0) {
            pr_err("download failed\n");
            return -1;
        }

        snprintf(resolved_path, len, "%s", dest);
        return 0;
    }
#endif

    /* Local file: use directly */
    if (access(url, F_OK) != 0) {
        pr_err("local image not found: %s\n", url);
        return -1;
    }

    snprintf(resolved_path, len, "%s", url);
    return 0;
}

/*
 * Get the directory containing the image (for sdboot dest paths).
 */
void source_ota_get_dir(const char *image_path, char *dir, size_t len)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", image_path);
    char *d = dirname(tmp);
    snprintf(dir, len, "%s", d);
}
