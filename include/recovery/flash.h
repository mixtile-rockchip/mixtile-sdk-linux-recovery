/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_FLASH_H
#define RK_RECOVERY_FLASH_H

#include "recovery/config.h"
#include "recovery/upgrade.h"

/* Normal partition flash: block or MTD, full or diff (auto-detect) */
int flash_normal(struct upgrade_item *item, const char *image_path);

#if defined(RK_DIFF_UPDATE)
/*
 * True (1) if the image slice [offset, offset+size) is a bsdiff patch rather
 * than a full image. A diff patches a partition's existing content in place,
 * so the engine rejects it for whole-disk / GPT-rewrite upgrades (no stable
 * base to patch). Returns 0 for a full image, -1 on read error.
 */
int is_diff_img(const char *img_path, long long offset, long long size);
#endif

#if defined(RK_IDBLOCK_UPDATE)
/* IDBlock bootloader flash */
int flash_bootloader(struct upgrade_item *item, const char *image_path);
#endif

#if defined(RK_GPT_UPDATE)
/* GPT parameter partition flash */
int flash_parameter(struct upgrade_item *item, const char *image_path);
#endif

#endif /* RK_RECOVERY_FLASH_H */
