/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 * Bootloader Control Block (BCB) and A/B slot metadata.
 *
 * MISC partition layout (DO NOT MODIFY offsets — must match uboot):
 *   0x0800 (2K):  AvbABData (32 bytes, with CRC32)
 *   0x1800 (6K):  CMD_WIPE_USERDATA command
 *   0x2800 (10K): Custom cmdline (2-byte length + max 1022 bytes data)
 *   0x4000 (16K): bootloader_message (BCB)
 */

#ifndef RK_RECOVERY_BOOTMSG_H
#define RK_RECOVERY_BOOTMSG_H

#include <stdint.h>
#include <stdbool.h>

/* ---- MISC partition offsets ---- */
#define MISC_OFFSET_AB          0x0800  /* AvbABData */
#define MISC_OFFSET_CMDLINE     0x1800  /* Wipe command */
#define MISC_OFFSET_CUSTOM      0x2800  /* Custom cmdline */
#define MISC_OFFSET_BCB         0x4000  /* bootloader_message */

#define AB_SLOT_NUM             2

/* ---- AVB A/B constants ---- */
#define AVB_AB_MAGIC            "\0AB0"
#define AVB_AB_MAGIC_LEN        4
#define AVB_AB_MAJOR_VERSION    1
#define AVB_AB_MINOR_VERSION    0
#define AVB_AB_DATA_SIZE        32
#define AVB_AB_MAX_PRIORITY     15
#define AVB_AB_MAX_TRIES_REMAINING 7

#define CMD_WIPE_USERDATA       "cmd_wipe_userdata"

/* ---- Bootloader message (BCB) ---- */
/*
 * WARNING: struct layout is ABI — must match u-boot include/boot_rkimg.h
 * byte-for-byte (1088 bytes). This is the AOSP-standard layout; the former
 * RK-private needupdate[4] + systemFlag[252] tail (which nothing outside
 * recovery ever read) was dropped in favor of the standard stage /
 * slot_suffix / reserved fields. Upgrade scope now travels in the recovery
 * args (--upgrade=), not a needupdate bitmask.
 */
struct bootloader_message {
    char command[32];
    char status[32];
    char recovery[768];
    char stage[32];
    char slot_suffix[32];
    char reserved[192];
};

/* ---- A/B slot data ---- */
struct avb_ab_slot_data {
    uint8_t priority;           /* 0 = unbootable, 1..15 */
    uint8_t tries_remaining;    /* 0..7 */
    uint8_t successful_boot;    /* 0 or 1 */
    uint8_t reserved[1];
};

/* WARNING: struct layout is ABI — must remain exactly 32 bytes */
struct avb_ab_data {
    uint8_t  magic[AVB_AB_MAGIC_LEN];
    uint8_t  version_major;
    uint8_t  version_minor;
    uint8_t  reserved1[2];
    struct avb_ab_slot_data slots[AB_SLOT_NUM];
    uint8_t  last_boot;         /* 0 = slot A, 1 = slot B */
    uint8_t  reserved2[11];
    uint32_t crc32;             /* CRC32 of preceding 28 bytes */
};

/* ---- BCB read/write (uses storage HAL internally) ---- */
struct storage_mgr;

int bcb_read(struct storage_mgr *mgr, struct bootloader_message *out);
int bcb_write(struct storage_mgr *mgr, const struct bootloader_message *in);
int bcb_clear(struct storage_mgr *mgr);

/* ---- A/B slot management ---- */
int  get_current_slot(void);    /* returns 0 (A), 1 (B), or -1 (non-AB) */
int  set_slot_activity(struct storage_mgr *mgr, int current_slot);
int  set_slot_succeed(struct storage_mgr *mgr);
void misc_display(struct storage_mgr *mgr);

/* A/B partition-name helpers (impl in core/slot.c) */
char get_ab_suffix(const char *name);          /* 'a' or 'b' for _a/_b names, else 0 */
bool slot_filter(char ab_suffix, int current_slot);  /* true = SKIP for this slot */

/* ---- MISC custom cmdline ---- */
int  misc_custom_write(struct storage_mgr *mgr);
int  misc_custom_read(struct storage_mgr *mgr);
int  misc_custom_clean(struct storage_mgr *mgr);

/* ---- Wipe userdata via MISC command ---- */
int  wipe_userdata(struct storage_mgr *mgr, int auto_reboot);

#endif /* RK_RECOVERY_BOOTMSG_H */
