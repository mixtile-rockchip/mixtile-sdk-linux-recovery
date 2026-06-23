/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "bootmsg"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/reboot.h>

#include "recovery/bootmsg.h"
#include "recovery/storage.h"
#include "recovery/device.h"
#include "recovery/config.h"
#include "recovery/log.h"

#define MISC_PART_NAME  "misc"

/* ---- Low-level MISC I/O via storage HAL ---- */

static int misc_read(struct storage_mgr *mgr, void *buf, off_t offset, size_t len)
{
    struct storage_ops *ops = storage_resolve(mgr, MISC_PART_NAME);
    if (!ops) {
        pr_err("cannot resolve misc partition\n");
        return -1;
    }

    char path[256];
    if (storage_get_part_path(mgr, MISC_PART_NAME, path, sizeof(path)) != 0) {
        pr_err("cannot get misc device path\n");
        return -1;
    }

    return ops->read(ops, path, buf, NULL, offset, (long long)len);
}

static int misc_write(struct storage_mgr *mgr, const void *buf, off_t offset, size_t len)
{
    struct storage_ops *ops = storage_resolve(mgr, MISC_PART_NAME);
    if (!ops) {
        pr_err("cannot resolve misc partition\n");
        return -1;
    }

    char path[256];
    if (storage_get_part_path(mgr, MISC_PART_NAME, path, sizeof(path)) != 0) {
        pr_err("cannot get misc device path\n");
        return -1;
    }

    /*
     * For misc writes we don't truncate — write at offset within existing
     * partition. Use O_RDWR semantics: pass offset, no O_TRUNC.
     * The storage backend's write() handles seeking.
     */
    return ops->write(ops, path, buf, NULL, 0, offset, (long long)len, NULL, NULL);
}

/* ---- BCB (bootloader_message at offset 0x4000) ---- */

int bcb_read(struct storage_mgr *mgr, struct bootloader_message *out)
{
    memset(out, 0, sizeof(*out));
    return misc_read(mgr, out, MISC_OFFSET_BCB, sizeof(*out));
}

int bcb_write(struct storage_mgr *mgr, const struct bootloader_message *in)
{
    return misc_write(mgr, in, MISC_OFFSET_BCB, sizeof(*in));
}

int bcb_clear(struct storage_mgr *mgr)
{
    struct bootloader_message msg;
    memset(&msg, 0, sizeof(msg));
    return bcb_write(mgr, &msg);
}

/* ---- Wipe userdata ---- */

int wipe_userdata(struct storage_mgr *mgr, int auto_reboot)
{
    int slot = get_current_slot();

    if (slot == -1) {
        /* Non-A/B: write BCB to trigger recovery --wipe_all */
        struct bootloader_message msg;
        memset(&msg, 0, sizeof(msg));
        snprintf(msg.command, sizeof(msg.command), "boot-recovery");
        snprintf(msg.recovery, sizeof(msg.recovery),
                 "recovery\n--wipe_all\n");
        if (bcb_write(mgr, &msg) != 0)
            return -1;
    } else {
        /* A/B: write wipe command at MISC_OFFSET_CMDLINE */
        if (misc_write(mgr, CMD_WIPE_USERDATA,
                       MISC_OFFSET_CMDLINE,
                       strlen(CMD_WIPE_USERDATA)) != 0)
            return -1;
    }

    if (auto_reboot) {
        pr_info("auto reboot for wipe_userdata\n");
        sleep(1);
        reboot(RB_AUTOBOOT);
    }
    return 0;
}

/* ---- Custom cmdline (offset 0x2800, format: 2-byte length + data) ---- */

#define CUSTOM_CMDLINE_PATH "/tmp/custom_cmdline"
#define CUSTOM_CMDLINE_MAX  1022

int misc_custom_read(struct storage_mgr *mgr)
{
    uint16_t len = 0;

    if (misc_read(mgr, &len, MISC_OFFSET_CUSTOM, sizeof(len)) != 0)
        return -1;

    if (len == 0 || len > CUSTOM_CMDLINE_MAX) {
        pr_err("custom cmdline invalid length: %u\n", len);
        return -1;
    }

    uint8_t *buf = malloc(len + 2);
    if (!buf)
        return -1;

    if (misc_read(mgr, buf, MISC_OFFSET_CUSTOM, len + 2) != 0) {
        free(buf);
        return -1;
    }

    int fd = open(CUSTOM_CMDLINE_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        pr_err("open %s: %s\n", CUSTOM_CMDLINE_PATH, strerror(errno));
        free(buf);
        return -1;
    }

    int ret = 0;
    if (write(fd, buf + 2, len) != (ssize_t)len) {
        pr_err("write %s: %s\n", CUSTOM_CMDLINE_PATH, strerror(errno));
        ret = -1;
    }

    close(fd);
    free(buf);
    return ret;
}

int misc_custom_write(struct storage_mgr *mgr)
{
    int fd = open(CUSTOM_CMDLINE_PATH, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", CUSTOM_CMDLINE_PATH, strerror(errno));
        return -1;
    }

    uint16_t len = 0;
    if (read(fd, &len, 2) != 2) {
        pr_err("read length from %s\n", CUSTOM_CMDLINE_PATH);
        close(fd);
        return -1;
    }

    if (len == 0 || len > CUSTOM_CMDLINE_MAX) {
        pr_err("custom cmdline length out of range: %u\n", len);
        close(fd);
        return -1;
    }

    uint8_t *buf = malloc(len + 2);
    if (!buf) {
        close(fd);
        return -1;
    }

    memcpy(buf, &len, 2);
    if (read(fd, buf + 2, len) != (ssize_t)len) {
        pr_err("read data from %s\n", CUSTOM_CMDLINE_PATH);
        free(buf);
        close(fd);
        return -1;
    }
    close(fd);

    int ret = misc_write(mgr, buf, MISC_OFFSET_CUSTOM, len + 2);
    free(buf);
    return ret;
}

int misc_custom_clean(struct storage_mgr *mgr)
{
    uint8_t zeros[1024];
    memset(zeros, 0, sizeof(zeros));
    return misc_write(mgr, zeros, MISC_OFFSET_CUSTOM, sizeof(zeros));
}
