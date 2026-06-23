/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "source"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "recovery/update_source.h"
#include "recovery/cmdline.h"
#include "recovery/config.h"
#include "recovery/log.h"

#define MAX_LINE 1024

/* ---- Static source table (cmdline key + mount paths + image name) ---- */

/*
 * Image candidate names, priority order (preserving original recovery_cmd.c
 * behavior: update_ab.img > update_ota.img > sdupdate.img):
 *   update_ab.img    — A/B OTA package
 *   update_ota.img   — OTA package (legacy)
 *   sdupdate.img     — SD/USB factory image (RK_DEFAULT_IMAGE)
 *   update.img       — generic
 * Vendor private names can override via `image=` line in sd_boot_config.config.
 */
#define RK_IMAGE_AB     "update_ab.img"
#define RK_IMAGE_OTA    "update_ota.img"
#define RK_IMAGE_LOCAL  "update.img"

static const struct update_source sources[] = {
#if defined(RK_SOURCE_SD)
    [SRC_SDCARD] = {
        .type = SRC_SDCARD,
        .cmdline_key = "sdfwupdate",
        .mount_paths = { RK_SD_MOUNT_POINT, NULL },
        .image_names = { RK_IMAGE_AB, RK_IMAGE_OTA, RK_DEFAULT_IMAGE,
                         RK_IMAGE_LOCAL, NULL },
    },
#endif
#if defined(RK_SOURCE_USB)
    [SRC_USB] = {
        .type = SRC_USB,
        .cmdline_key = "usbfwupdate",
        .mount_paths = { RK_USB_MOUNT_POINT, NULL },
        .image_names = { RK_IMAGE_AB, RK_IMAGE_OTA, RK_DEFAULT_IMAGE,
                         RK_IMAGE_LOCAL, NULL },
    },
#endif
    [SRC_LOCAL] = {
        .type = SRC_LOCAL,
        .cmdline_key = NULL,
        .mount_paths = { "/cache", "/userdata", NULL },
        .image_names = { RK_IMAGE_LOCAL, NULL },
    },
};

static const struct update_source *get_source_def(enum update_src_type type)
{
    if ((size_t)type >= sizeof(sources) / sizeof(sources[0]))
        return NULL;
    /* Unconfigured entries (e.g. SD/USB trimmed out) have a NULL mount_paths[0] */
    const struct update_source *s = &sources[type];
    if (type != SRC_LOCAL && !s->cmdline_key)
        return NULL;
    return s;
}

/*
 * Check sd_boot_config.config for an `image=xxx` override entry.
 * Returns 0 if found (out filled), -1 otherwise.
 * Only applies to SD/USB sources — LOCAL has no config file.
 */
static int read_image_override(const struct update_source *src,
                               char *out, size_t len)
{
    if (!src || src->type == SRC_LOCAL)
        return -1;
    return source_get_cfg_item(src, "image", out, len);
}

/* ---- Self-mount helpers: discover boot media + mount it ourselves ----
 *
 * Vendor fstab on rk3588 hard-codes /dev/sda1 /mnt/udisk + /dev/mmcblk1p1
 * /mnt/sdcard, which fails on this board: SATA SSD takes sda (so the
 * U-disk enumerates as sdb), and the SD card may not be present at all.
 * Don't rely on fstab — find the boot media in /sys/block ourselves and
 * mount it at one of src->mount_paths.
 */

static bool is_path_mounted(const char *path)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return false;
    char line[512], mnt[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%*s %255s", mnt) == 1 && strcmp(mnt, path) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* True if /sys/block/<dev> resolves into a USB host (vs ATA/SATA). */
static bool block_is_usb(const char *dev_name)
{
    char link_path[256];
    char target[512];
    snprintf(link_path, sizeof(link_path), "/sys/block/%s", dev_name);
    ssize_t n = readlink(link_path, target, sizeof(target) - 1);
    if (n <= 0) return false;
    target[n] = '\0';
    /* USB sysfs path contains "/usb" e.g. .../usb8/8-1/8-1:1.0/host1/... */
    return strstr(target, "/usb") != NULL;
}

/* True if /sys/block/mmcblk<N>/device/type == "SD". eMMC has type "MMC". */
static bool block_is_sd(const char *dev_name)
{
    char type_path[256];
    snprintf(type_path, sizeof(type_path),
             "/sys/block/%s/device/type", dev_name);
    FILE *f = fopen(type_path, "r");
    if (!f) return false;
    char type[16] = {0};
    if (!fgets(type, sizeof(type), f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    /* Strip trailing newline */
    size_t l = strlen(type);
    if (l && type[l - 1] == '\n') type[l - 1] = '\0';
    return strcmp(type, "SD") == 0;
}

/* Find first partition under /sys/block/<parent> (e.g. sdb1, mmcblk1p1). */
static int first_partition(const char *parent, char *out, size_t out_len)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "/sys/block/%s", parent);
    DIR *d = opendir(dir);
    if (!d) return -1;

    int found = -1;
    size_t plen = strlen(parent);
    struct dirent *e;
    while ((e = readdir(d))) {
        /* Partitions are subdirs whose name starts with parent + extra chars */
        if (strncmp(e->d_name, parent, plen) != 0) continue;
        if (strlen(e->d_name) <= plen) continue;
        snprintf(out, out_len, "/dev/%s", e->d_name);
        found = 0;
        break;      /* take the first one — typically p1 */
    }
    closedir(d);
    return found;
}

/*
 * Find the boot media partition device path for the given source type.
 *   SRC_USB    -> first /sys/block/sd[a-z] whose path contains "/usb",
 *                 then its first partition (e.g. /dev/sdb1)
 *   SRC_SDCARD -> first /sys/block/mmcblkN whose device/type == "SD",
 *                 then its first partition (e.g. /dev/mmcblk1p1)
 */
static int find_boot_media(enum update_src_type type,
                           char *dev_out, size_t out_len)
{
    DIR *d = opendir("/sys/block");
    if (!d) return -1;

    int rc = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (type == SRC_USB) {
            /* whole sd[a-z] disks: 3 chars, starts with "sd" */
            if (strlen(e->d_name) != 3 || strncmp(e->d_name, "sd", 2) != 0)
                continue;
            if (!block_is_usb(e->d_name)) continue;
        } else if (type == SRC_SDCARD) {
            if (strncmp(e->d_name, "mmcblk", 6) != 0) continue;
            /* skip partition entries like mmcblk1p1 */
            const char *p = e->d_name + 6;
            while (*p && *p >= '0' && *p <= '9') p++;
            if (*p != '\0') continue;       /* has non-digit suffix → partition */
            if (!block_is_sd(e->d_name)) continue;
        } else {
            continue;
        }

        if (first_partition(e->d_name, dev_out, out_len) == 0) {
            rc = 0;
            break;
        }
    }
    closedir(d);
    return rc;
}

/* Try to mount `dev` at `mnt`, sweeping common removable-media fs types. */
static int try_mount_fs(const char *dev, const char *mnt)
{
    static const char *fs_types[] = {
        "vfat", "exfat", "ntfs", "ext4", "ext3", "ext2", NULL
    };
    mkdir(mnt, 0755);   /* ensure mount point exists; ignore EEXIST */

    for (int i = 0; fs_types[i]; i++) {
        if (mount(dev, mnt, fs_types[i], MS_RDONLY, NULL) == 0) {
            pr_info("mounted %s -> %s (fs=%s)\n", dev, mnt, fs_types[i]);
            return 0;
        }
        pr_dbg("mount %s -> %s as %s: %s\n",
               dev, mnt, fs_types[i], strerror(errno));
    }
    return -1;
}

/*
 * Ensure the source media is mounted at one of src->mount_paths.
 *   - LOCAL: no-op (fstab handles /userdata, /oem, etc).
 *   - SD/USB: if already mounted at any candidate, OK; otherwise discover
 *     the boot media block device and mount it at mount_paths[0].
 *
 * Returns 0 on success (mount available), -1 if no media or mount failed.
 */
int source_ensure_mounted(enum update_src_type type)
{
    const struct update_source *src = get_source_def(type);
    if (!src || src->type == SRC_LOCAL)
        return 0;

    /*
     * Poll for the boot media, handling two race conditions:
     *   1. USB storage appears late — kernel logs usb-storage attach
     *      before scsi finishes probing the LUN, so /sys/block/sdN may
     *      not exist yet when recovery starts.
     *   2. Once the device appears, udev / systemd-mount / vendor
     *      auto-mount may grab the partition and mount it at one of our
     *      candidate paths before we get a chance. EBUSY from our own
     *      mount() then means "someone already mounted it", not "fs
     *      type wrong". Re-check the candidate paths each tick instead
     *      of fighting them.
     */
    char dev[64];
    bool media_found = false;
    const int kStepMs = 500;
    const int kMaxMs  = 5000;
    for (int waited = 0; waited <= kMaxMs; waited += kStepMs) {
        for (int i = 0; i < 4 && src->mount_paths[i]; i++) {
            if (is_path_mounted(src->mount_paths[i])) {
                pr_info("source mounted at %s\n", src->mount_paths[i]);
                return 0;
            }
        }
        if (find_boot_media(src->type, dev, sizeof(dev)) == 0) {
            media_found = true;
            break;
        }
        if (waited == 0)
            pr_info("waiting for %s media to appear in /sys/block...\n",
                    src->type == SRC_USB ? "USB" : "SD");
        usleep(kStepMs * 1000);
    }
    if (!media_found) {
        pr_err("no %s media found in /sys/block after %dms\n",
               src->type == SRC_USB ? "USB" : "SD", kMaxMs);
        return -1;
    }
    pr_info("found %s media: %s\n",
            src->type == SRC_USB ? "USB" : "SD", dev);

    /* Final check: an auto-mounter may have grabbed the device between
     * find_boot_media succeeding and us calling mount. */
    for (int i = 0; i < 4 && src->mount_paths[i]; i++) {
        if (is_path_mounted(src->mount_paths[i])) {
            pr_info("source mounted at %s by auto-mounter\n",
                    src->mount_paths[i]);
            return 0;
        }
    }

    if (try_mount_fs(dev, src->mount_paths[0]) != 0) {
        pr_err("failed to mount %s -> %s (tried vfat/exfat/ntfs/ext)\n",
               dev, src->mount_paths[0]);
        return -1;
    }
    return 0;
}

/* ---- Public API ---- */

bool source_is_boot(enum update_src_type type)
{
    const struct update_source *src = get_source_def(type);
    if (!src || !src->cmdline_key)
        return false;

    if (cmdline_contains(src->cmdline_key)) {
        pr_info("boot from %s\n", src->cmdline_key);
        return true;
    }
    return false;
}

bool source_has_update(enum update_src_type type)
{
    const struct update_source *src = get_source_def(type);
    if (!src)
        return false;

    char value[16] = { 0 };
    if (source_get_cfg_item(src, "fw_update", value, sizeof(value)) != 0)
        return false;

    if (strcmp(value, "1") == 0) {
        pr_info("%s firmware update requested\n",
                src->cmdline_key ? src->cmdline_key : "local");
        return true;
    }
    return false;
}

/* Parse a simple key=value config file. Returns 0 on success, -1 on error. */
int source_get_cfg_item(const struct update_source *src, const char *key,
                        char *value, size_t len)
{
    char config_path[256];
    FILE *fp;
    char line[MAX_LINE];

    if (!src || !src->mount_paths[0]) {
        pr_err("no mount path for source\n");
        return -1;
    }

    snprintf(config_path, sizeof(config_path), "%s/sd_boot_config.config",
             src->mount_paths[0]);

    fp = fopen(config_path, "r");
    if (!fp) {
        pr_dbg("config file not found: %s\n", config_path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq)
            continue;

        char *k = strstr(line, key);
        if (!k || k > eq)
            continue;

        char *v = eq + 1;
        while (*v == ' ')
            v++;

        char *end = v + strlen(v) - 1;
        while (end > v && (*end == '\n' || *end == '\r' || *end == ' '))
            *end-- = '\0';

        if (*v == '\0' || *v == '\n')
            continue;

        size_t vlen = strlen(v);
        if (vlen >= len)
            vlen = len - 1;
        memcpy(value, v, vlen);
        value[vlen] = '\0';

        fclose(fp);
        return 0;
    }

    fclose(fp);
    return -1;
}

int source_detect_active(struct update_source *out)
{
    if (!out)
        return -1;

#if defined(RK_SOURCE_SD)
    if (source_is_boot(SRC_SDCARD)) {
        *out = *get_source_def(SRC_SDCARD);
        pr_info("detected source: sdcard\n");
        return 0;
    }
#endif
#if defined(RK_SOURCE_USB)
    if (source_is_boot(SRC_USB)) {
        *out = *get_source_def(SRC_USB);
        pr_info("detected source: usb\n");
        return 0;
    }
#endif

    *out = *get_source_def(SRC_LOCAL);
    pr_info("detected source: local\n");
    return 0;
}

int source_get_image_path(const struct update_source *src,
                          char *path, size_t len)
{
    if (!src || !src->mount_paths[0] || !src->image_names[0])
        return -1;

    /* Vendor override: `image=xxx` in sd_boot_config.config (SD/USB only).
     * If present, try it first across every mount path. */
    char override[64] = {0};
    bool has_override = (read_image_override(src, override, sizeof(override)) == 0
                         && override[0]);

    for (int mi = 0; mi < 4 && src->mount_paths[mi]; mi++) {
        if (has_override) {
            snprintf(path, len, "%s/%s", src->mount_paths[mi], override);
            if (access(path, F_OK) == 0) {
                pr_info("found image (vendor override): %s\n", path);
                return 0;
            }
        }
        for (int ni = 0; ni < MAX_IMAGE_NAMES && src->image_names[ni]; ni++) {
            snprintf(path, len, "%s/%s",
                     src->mount_paths[mi], src->image_names[ni]);
            if (access(path, F_OK) == 0) {
                pr_info("found image: %s\n", path);
                return 0;
            }
        }
    }

    pr_warn("image not found in any (mount_path, image_name) combo\n");
    return -1;
}
