/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_MTD_INFO_H
#define RK_RECOVERY_MTD_INFO_H

#include <stdbool.h>

/*
 * Look up the MTD partition number for a given name by scanning /proc/mtd.
 *
 * Returns the MTD index (0..N) on success, or -1 when /proc/mtd is
 * unreadable or no entry matches.
 */
int mtd_find_num_by_name(const char *part_name);

/* Convenience wrapper: mtd_find_num_by_name(...) >= 0. */
bool mtd_name_exists(const char *part_name);

/* Returns true if /proc/mtd has at least one mtdN: entry. */
bool mtd_any_exists(void);

/*
 * Extract the numeric index from a device path. Accepts /dev/mtdN,
 * /dev/mtdblockN, or bare mtdN / mtdblockN basenames.
 *
 * Returns N on success, or -1 if the path does not match.
 */
int mtd_parse_dev_num(const char *mtd_path);

/*
 * Find the UBI device index attached to a given MTD index by walking
 * /sys/class/ubi/ubiN/mtd_num.
 *
 * Returns the UBI index on success, or -1 if no UBI device is attached.
 */
int ubi_find_num_by_mtd(int mtd_num);

/*
 * True if the MTD at dev_path is a NOR flash (`/sys/class/mtd/mtdN/type`
 * reads "nor"). NOR has no OOB / no bad-block table, so `nanddump` and
 * the MTDFILEMODE ioctl are not supported and reads must go through
 * plain read() on the char device.
 *
 * Accepts /dev/mtdN, /dev/mtdblockN, or bare mtdN / mtdblockN.
 * Returns false when type cannot be read or is anything other than "nor".
 */
bool mtd_is_nor(const char *dev_path);

/*
 * Find the whole-chip MTD master device of the given flavor: the /proc/mtd
 * entry with no /sys/class/mtd/mtdN/offset attribute (the kernel attaches
 * `offset` to partitions only) whose type matches want_nor.
 *
 * With mtdparts in the cmdline the master is only retained when the kernel
 * sets CONFIG_MTD_PARTITIONED_MASTER=y; without mtdparts (SD/USB boot) the
 * sole mtdN is the master trivially.
 *
 * Returns the MTD index, or -1 when no master is visible.
 */
int mtd_find_master(bool want_nor);

#endif /* RK_RECOVERY_MTD_INFO_H */
