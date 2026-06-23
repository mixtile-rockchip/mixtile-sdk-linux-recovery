/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_CONFIG_H
#define RK_RECOVERY_CONFIG_H

/* ---- Paths ---- */
#ifndef RK_BLOCK_BY_NAME
#define RK_BLOCK_BY_NAME    "/dev/block/by-name"
#endif
#ifndef RK_FSTAB_PATH
#define RK_FSTAB_PATH       "/etc/fstab"
#endif
#ifndef RK_RECOVERY_LOG
#define RK_RECOVERY_LOG     "/tmp/recovery.log"
#endif
#ifndef RK_SD_MOUNT_POINT
#define RK_SD_MOUNT_POINT   "/mnt/sdcard"
#endif
#ifndef RK_USB_MOUNT_POINT
#define RK_USB_MOUNT_POINT  "/mnt/udisk"   /* matches vendor fstab convention */
#endif
#ifndef RK_DEFAULT_IMAGE
#define RK_DEFAULT_IMAGE    "sdupdate.img"
#endif

/* ---- Misc constants ---- */
#define BLOCK_WRITE_LEN     (16 * 1024)

/*
 * The Rockchip format unit — NOT a device property. RKBOOT IDBlock sectors,
 * update.img item.flash_offset and parameter.txt mtdparts offsets are all
 * defined by the rk tooling as counts of 512-byte sectors (u-boot likewise
 * keeps a fixed blksz=512 space for them, even forcing it on 4K UFS). A
 * device's real logical block is a separate concept — see the storage_ops
 * disk_lbas pair.
 */
#define SECTOR_SIZE         512
#define IDB_BLOCKS          5
#define IDBLOCK_TOP         50
#define MAX_PARTS           20

/*
 * Upper bound for any single loader-entry payload inside an IDBlock image.
 * Rejects malformed/malicious headers that would otherwise drive an
 * arbitrary-size malloc in get_loader_data().
 */
#define RK_LOADER_MAX_DATA  (16 * 1024 * 1024)

#define BYTE2SECTOR(x)      ((x) > 0 ? (((x) - 1) / SECTOR_SIZE + 1) : (x))

#endif /* RK_RECOVERY_CONFIG_H */
