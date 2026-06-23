/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_DEVICE_H
#define RK_RECOVERY_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

/* Storage type detection from /proc/cmdline (cmdline `storagemedia=` — mutually exclusive) */
bool device_is_mtd(void);
bool device_is_ufs(void);

/*
 * Existence probes — non-exclusive runtime evidence.
 *
 * device_has_mtd:  /proc/mtd has at least one entry
 * device_has_nand: a /sys/class/mtd/mtdN type contains "nand"
 * device_has_nor:  a /sys/class/mtd/mtdN type contains "nor"
 * device_has_emmc: /sys/bus/mmc/devices has a child with type=MMC
 * device_has_ufs:  proc_name of any scsi_host contains "ufs" (UFS HC)
 * device_has_sata: proc_name of any scsi_host contains "ahci/sata/ata"
 *
 * UFS and SATA are strictly distinguished even though both look like SCSI
 * (/dev/sd*). UFS is bootable (IDBlock at /dev/sdb boot LUN); SATA is not
 * (must pair with Nor for IDBlock). NAND and NOR are likewise distinguished
 * (different write tooling + IDBlock layout). The per-medium storage
 * backends' bootloader_dev fields use these distinctions.
 *
 * Dual-storage boards (rk3588 Nor+SATA: storagemedia=scsi yet Nor still
 * attached after dts enable) need multiple has_* to be true simultaneously.
 */
bool device_has_mtd(void);
bool device_has_nand(void);
bool device_has_nor(void);
bool device_has_emmc(void);
bool device_has_ufs(void);
bool device_has_sata(void);

/* Get flash device paths */
const char *device_get_flash_dev(void);

/* A/B slot detection from cmdline: 0=A, 1=B, -1=non-AB/recovery */
int device_get_current_slot(void);

/* Get serial console path (e.g. "/dev/ttyS2") */
int device_get_serial(char *buf, size_t len);

/* Initialize MMC device points (scans /sys/bus/mmc/devices/) */
void device_init_mmc_points(void);
const char *device_get_emmc_point(void);
const char *device_get_sd_point(void);

#endif /* RK_RECOVERY_DEVICE_H */
