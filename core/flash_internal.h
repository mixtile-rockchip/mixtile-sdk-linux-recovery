/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_CORE_FLASH_INTERNAL_H
#define RK_RECOVERY_CORE_FLASH_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include "recovery/config.h"
#include "recovery/upgrade.h"

struct storage_ops;
struct storage_mgr;

/* ---- Diff helpers (flash_diff.c) ---- */
#if defined(RK_DIFF_UPDATE)

#define DIFF_TAIL_SIZE  80
#define DIFF_MAGIC      "DIFF"
#define UBI_HEAD_MAGIC  "UBI#"

/* is_diff_img is declared in the public recovery/flash.h (the engine queries
 * it to reject diffs on whole-disk/GPT upgrades). */
bool is_mtd_ubi(const char *mtd_path);
bool is_ubi_image(const char *img_path, long long offset);
long long parse_diff_old_size(const char *img_path,
                              long long offset, long long size);
int  mtd_read_to_file(struct storage_ops *ops, const char *mtd_path,
                      const char *dst_file, long long size);

int  flash_block_diff(struct upgrade_item *item, const char *image_path);
int  flash_mtd_ubi_diff(struct upgrade_item *item, const char *image_path);
int  flash_mtd_raw_diff(struct upgrade_item *item, const char *image_path);

#endif /* RK_DIFF_UPDATE */

/* IDBlock loader lives in flash_bootloader.c: download_loader is static there,
 * driven by the public flash_bootloader() entry (recovery/flash.h). */

/* ---- bsdiff patcher (patch.c) ---- */
int  do_patch_rkimg(const char *img, long long offset, long long size,
                    const char *blk_dev, const char *dst_file);

#endif /* RK_RECOVERY_CORE_FLASH_INTERNAL_H */
