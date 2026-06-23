/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_DOWNLOAD_H
#define RK_RECOVERY_DOWNLOAD_H

/*
 * Place the content addressed by `url` at `save_path` and return 0.
 *
 *   url      : "http(s)://..." for remote, otherwise a local path.
 *   save_path: destination. If NULL/empty, or identical to `url`, the image
 *              is used in place — no copy, no download.
 *
 * Returns 0 on success (image is present at the effective target), -1 on
 * any failure. HTTP URLs require RK_SOURCE_URL at build time; without it
 * an http(s) url returns -1.
 *
 * Used by upgrade_run() so both flows look the same to the caller and the
 * two-step BCB path (which writes save_path) always finds a real file.
 */
int download_file(const char *url, const char *save_path);

#endif /* RK_RECOVERY_DOWNLOAD_H */
