/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2024 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "device"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "recovery/device.h"
#include "recovery/log.h"
#include "recovery/config.h"
#include "recovery/mtd_info.h"
#include "recovery/cmdline.h"

#define UFS_SDA_NAME    "/dev/sda"
#define UFS_SDB_NAME    "/dev/sdb"
#define UFS_SDC_NAME    "/dev/sdc"
#define UFS_SDD_NAME    "/dev/sdd"

#define NAND_DEV_LBA    "/dev/mtd0"

/* ---- Public API ---- */

/* ---- Existence probes (non-exclusive) ----
 *
 * Unlike `device_is_*` (which reflect cmdline `storagemedia=` and so are
 * mutually exclusive — what the boot source claims), these look at runtime
 * /proc and /sys evidence. A dual-storage board reports multiple as true.
 */

bool device_has_mtd(void)
{
    /* /proc/mtd has a header line + one line per partition. Any data
     * beyond the header means at least one MTD partition is exposed. */
    int fd = open("/proc/mtd", O_RDONLY);
    if (fd < 0) {
        pr_dbg("device_has_mtd: /proc/mtd not present\n");
        return false;
    }
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        pr_dbg("device_has_mtd: /proc/mtd empty\n");
        return false;
    }
    buf[n] = '\0';
    /* Look for a line beginning with "mtd" (after the header) */
    bool found = (strstr(buf, "\nmtd") != NULL);
    pr_dbg("device_has_mtd: %s\n", found ? "yes" : "no");
    return found;
}

bool device_has_emmc(void)
{
    /* MMC subsystem exposes attached devices under /sys/bus/mmc/devices/.
     * Each child whose `type` reads "MMC" is an eMMC; "SD" is a removable
     * card and doesn't count as eMMC for our purpose. */
    DIR *dir = opendir("/sys/bus/mmc/devices/");
    if (!dir) {
        pr_dbg("device_has_emmc: /sys/bus/mmc/devices not present\n");
        return false;
    }

    struct dirent *de;
    bool found = false;
    while (!found && (de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "mmc", 3) != 0)
            continue;

        char type_path[PATH_MAX];
        snprintf(type_path, sizeof(type_path), "%s/type", de->d_name);
        int fd = openat(dirfd(dir), type_path, O_RDONLY);
        if (fd < 0)
            continue;

        char tbuf[16] = {0};
        ssize_t n = read(fd, tbuf, sizeof(tbuf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        for (ssize_t i = 0; i < n; i++)
            if (tbuf[i] == '\n') { tbuf[i] = '\0'; break; }

        if (strcmp(tbuf, "MMC") == 0)
            found = true;
    }
    closedir(dir);
    pr_dbg("device_has_emmc: %s\n", found ? "yes" : "no");
    return found;
}

/*
 * UFS and SATA both register as SCSI hosts (kernel exposes /dev/sd*), but
 * they have very different semantics:
 *
 *   UFS  is bootable on its own. The chip exposes multiple LUNs sda/sdb/...,
 *        and the bootrom reads IDBlock from the boot LUN (typically sdb).
 *
 *   SATA is NOT bootable. SATA boards must pair with a bootable storage
 *        (Nor flash). IDBlock lives on Nor, not on the SATA disk.
 *
 * So the backends' bootloader_dev field must hold different things --
 * /dev/sdb for UFS, NULL for SATA (the IDBlock belongs to the Nor backend
 * there). Distinguish by
 * reading the proc_name attribute of any /sys/class/scsi_host/hostN node:
 *   "ufshcd" / "ufs_qcom" / ...     -> UFS
 *   "ahci"   / "sata_*" / "ata_piix"-> SATA
 */
enum scsi_subtype { SCSI_NONE, SCSI_UFS, SCSI_SATA };

static enum scsi_subtype probe_scsi_subtype(void)
{
    DIR *dir = opendir("/sys/class/scsi_host/");
    if (!dir)
        return SCSI_NONE;

    enum scsi_subtype result = SCSI_NONE;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "host", 4) != 0)
            continue;

        char pn_path[PATH_MAX];
        snprintf(pn_path, sizeof(pn_path), "%s/proc_name", de->d_name);
        int fd = openat(dirfd(dir), pn_path, O_RDONLY);
        if (fd < 0)
            continue;

        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] == '\n') { buf[i] = '\0'; break; }

        /* UFS first — "ufshcd"/"ufs_..." */
        if (strstr(buf, "ufs")) {
            result = SCSI_UFS;
            break;  /* UFS is authoritative; no need to keep looking */
        }
        /* SATA: ahci / sata_xxx / ata_piix / pata_xxx */
        if (strstr(buf, "ahci") || strstr(buf, "sata") || strstr(buf, "ata")) {
            result = SCSI_SATA;
            /* keep scanning in case a later host turns out to be UFS */
        }
    }
    closedir(dir);

    /* Require /dev/sda to exist regardless of host type */
    if (result != SCSI_NONE && access("/dev/sda", F_OK) != 0)
        result = SCSI_NONE;

    return result;
}

bool device_has_ufs(void)
{
    bool found = (probe_scsi_subtype() == SCSI_UFS);
    pr_dbg("device_has_ufs: %s\n", found ? "yes" : "no");
    return found;
}

bool device_has_sata(void)
{
    bool found = (probe_scsi_subtype() == SCSI_SATA);
    pr_dbg("device_has_sata: %s\n", found ? "yes" : "no");
    return found;
}

/*
 * NAND vs NOR presence. Both are MTD char devices (/dev/mtdN); the medium is
 * read from /sys/class/mtd/mtdN/type. A board may carry both (e.g. SPI-NAND +
 * SPI-NOR), so these are non-exclusive. Matched by substring so "mlc-nand"
 * and "stm-nor" classify correctly; "nand" and "nor" never cross-match.
 */
static bool any_mtd_type_contains(const char *kw)
{
    DIR *dir = opendir("/sys/class/mtd");
    if (!dir)
        return false;

    struct dirent *de;
    bool found = false;
    while (!found && (de = readdir(dir)) != NULL) {
        /* Accept "mtd<N>" only: prefix "mtd" then all digits (skip mtdNro). */
        if (strncmp(de->d_name, "mtd", 3) != 0 || de->d_name[3] == '\0')
            continue;
        bool all_digits = true;
        for (const char *q = de->d_name + 3; *q; q++) {
            if (*q < '0' || *q > '9') { all_digits = false; break; }
        }
        if (!all_digits)
            continue;

        char type_path[PATH_MAX];
        snprintf(type_path, sizeof(type_path),
                 "/sys/class/mtd/%s/type", de->d_name);
        int fd = open(type_path, O_RDONLY);
        if (fd < 0)
            continue;
        char tbuf[32] = {0};
        ssize_t n = read(fd, tbuf, sizeof(tbuf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        tbuf[n] = '\0';
        if (strstr(tbuf, kw))
            found = true;
    }
    closedir(dir);
    return found;
}

bool device_has_nand(void)
{
    bool found = any_mtd_type_contains("nand");
    pr_dbg("device_has_nand: %s\n", found ? "yes" : "no");
    return found;
}

bool device_has_nor(void)
{
    bool found = any_mtd_type_contains("nor");
    pr_dbg("device_has_nor: %s\n", found ? "yes" : "no");
    return found;
}

bool device_is_mtd(void)
{
    char val[64];
    if (cmdline_get_value("storagemedia", val, sizeof(val)) < 0) {
        pr_dbg("no storagemedia in cmdline, default not MTD\n");
        return false;
    }

    if (strncmp(val, "mtd", 3) == 0) {
        pr_dbg("storagemedia=mtd\n");
        return true;
    }

    if (strncmp(val, "sd", 2) == 0) {
        /* SD boot: MTD may still be present alongside; probe /proc/mtd. */
        if (mtd_any_exists())
            return true;
    }

    return false;
}

bool device_is_ufs(void)
{
    if (cmdline_contains("storagemedia=scsi")) {
        pr_dbg("storagemedia=scsi → UFS\n");
        return true;
    }

    bool has_sd_or_usb = (cmdline_contains("storagemedia=sd") && cmdline_contains("sdfwupdate")) ||
                         (cmdline_contains("storagemedia=usb") && cmdline_contains("usbfwupdate"));
    if (has_sd_or_usb) {
        if (access(UFS_SDA_NAME, F_OK) == 0 && access(UFS_SDB_NAME, F_OK) == 0 &&
            access(UFS_SDC_NAME, F_OK) == 0 && access(UFS_SDD_NAME, F_OK) == 0) {
            pr_dbg("SD/USB boot with UFS device nodes\n");
            return true;
        }
    }

    return false;
}

const char *device_get_flash_dev(void)
{
    if (device_is_ufs())
        return UFS_SDA_NAME;

    const char *emmc_point = device_get_emmc_point();
    if (emmc_point[0] && access(emmc_point, F_OK) == 0)
        return emmc_point;

    if (device_is_mtd())
        return NAND_DEV_LBA;

    return "/dev/mmcblk0";
}

int device_get_current_slot(void)
{
    char val[16];
    if (cmdline_get_value("androidboot.slot_suffix", val, sizeof(val)) < 0 &&
        cmdline_get_value("android_slotsufix", val, sizeof(val)) < 0)
        return -1;

    const char *p = val;
    if (*p == '_')
        p++;

    if (*p == 'a')
        return 0;
    if (*p == 'b')
        return 1;
    return -1;
}

int device_get_serial(char *buf, size_t len)
{
    if (!buf || len < 6)
        return -1;

    char console[64];
    if (cmdline_get_value("console", console, sizeof(console)) < 0)
        return -1;

    int n = snprintf(buf, len, "/dev/%s", console);
    if (n < 0 || (size_t)n >= len)
        return -1;
    return 0;
}

/* ---- MMC device point detection ---- */

enum mmc_type {
    MMC_TYPE_MMC,
    MMC_TYPE_SD,
    MMC_TYPE_SDIO,
    MMC_TYPE_SDCOMBO,
    MMC_TYPE_MAX,
};

static const char *mmc_type_names[] = { "MMC", "SD", "SDIO", "SDcombo" };
static const char *mmc_dev_names[] = {
    "/dev/mmcblk0", "/dev/mmcblk1", "/dev/mmcblk2", "/dev/mmcblk3"
};

static char mmc_points[MMC_TYPE_MAX][20];

void device_init_mmc_points(void)
{
    DIR *dir = opendir("/sys/bus/mmc/devices/");
    if (!dir)
        return;

    memset(mmc_points, 0, sizeof(mmc_points));

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "mmc", 3) != 0)
            continue;

        char type_path[PATH_MAX];
        snprintf(type_path, sizeof(type_path), "%s/type", de->d_name);

        int fd = openat(dirfd(dir), type_path, O_RDONLY);
        if (fd < 0)
            continue;

        char type_buf[16] = {0};
        int n = read(fd, type_buf, sizeof(type_buf) - 1);
        close(fd);
        if (n <= 0)
            continue;

        /* Strip newline */
        for (int i = 0; i < n; i++) {
            if (type_buf[i] == '\n') {
                type_buf[i] = '\0';
                break;
            }
        }

        int dev_idx = de->d_name[3] - '0';
        if (dev_idx < 0 || dev_idx >= 4)
            continue;

        for (int t = 0; t < MMC_TYPE_MAX; t++) {
            if (strcmp(mmc_type_names[t], type_buf) == 0) {
                snprintf(mmc_points[t], sizeof(mmc_points[t]), "%s",
                         mmc_dev_names[dev_idx]);
                break;
            }
        }
    }
    closedir(dir);

    pr_dbg("MMC points: emmc=%s sd=%s\n", mmc_points[MMC_TYPE_MMC],
           mmc_points[MMC_TYPE_SD]);
}

const char *device_get_emmc_point(void)
{
    return mmc_points[MMC_TYPE_MMC];
}

const char *device_get_sd_point(void)
{
    return mmc_points[MMC_TYPE_SD];
}
