/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "slot"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "recovery/bootmsg.h"
#include "recovery/storage.h"
#include "recovery/device.h"
#include "recovery/log.h"

/* ---- AVB CRC32 (same table as original rkbootloader.c) ---- */

static const uint32_t avb_crc32_tab[] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

static uint32_t avb_crc32_calc(const uint8_t *buf, size_t size)
{
    uint32_t crc = ~0U;
    while (size--)
        crc = avb_crc32_tab[(crc ^ *buf++) & 0xFF] ^ (crc >> 8);
    return crc ^ ~0U;
}

/* Host-to-big-endian for CRC32 storage (matches uboot convention) */
static uint32_t avb_htobe32(uint32_t in)
{
    union {
        uint32_t word;
        uint8_t  bytes[4];
    } ret;
    ret.bytes[0] = (in >> 24) & 0xff;
    ret.bytes[1] = (in >> 16) & 0xff;
    ret.bytes[2] = (in >> 8) & 0xff;
    ret.bytes[3] = in & 0xff;
    return ret.word;
}

static void avb_ab_update_crc(struct avb_ab_data *data)
{
    data->crc32 = avb_htobe32(
        avb_crc32_calc((const uint8_t *)data,
                       sizeof(struct avb_ab_data) - sizeof(uint32_t)));
}

static int avb_ab_verify_crc(const struct avb_ab_data *data)
{
    uint32_t expected = avb_htobe32(
        avb_crc32_calc((const uint8_t *)data,
                       sizeof(struct avb_ab_data) - sizeof(uint32_t)));
    return data->crc32 == expected ? 0 : -1;
}

/* Constant-time comparison (from AOSP avb) */
static int avb_safe_memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *a = s1, *b = s2;
    int result = 0;
    while (n--)
        result |= *a++ ^ *b++;
    return result != 0;
}

/* ---- MISC partition helpers ---- */

#define MISC_PART_NAME "misc"

static int read_ab_misc(struct storage_mgr *mgr, struct avb_ab_data *info)
{
    struct storage_ops *ops = storage_resolve(mgr, MISC_PART_NAME);
    if (!ops) {
        pr_err("misc: no backend for '%s'\n", MISC_PART_NAME);
        return -1;
    }

    char path[256];
    if (storage_get_part_path(mgr, MISC_PART_NAME, path, sizeof(path)) != 0) {
        pr_err("misc: cannot resolve device path for '%s'\n", MISC_PART_NAME);
        return -1;
    }

    int ret = ops->read(ops, path, info, NULL, MISC_OFFSET_AB, (long long)sizeof(*info));
    if (ret != 0)
        return ret;

    if (avb_ab_verify_crc(info) != 0) {
        pr_err("misc: A/B data CRC mismatch — corrupted misc partition\n");
        return -1;
    }
    return 0;
}

static int write_ab_misc(struct storage_mgr *mgr, const struct avb_ab_data *info)
{
    struct storage_ops *ops = storage_resolve(mgr, MISC_PART_NAME);
    if (!ops) {
        pr_err("misc: no backend for '%s'\n", MISC_PART_NAME);
        return -1;
    }

    char path[256];
    if (storage_get_part_path(mgr, MISC_PART_NAME, path, sizeof(path)) != 0) {
        pr_err("misc: cannot resolve device path for '%s'\n", MISC_PART_NAME);
        return -1;
    }

    return ops->write(ops, path, info, NULL, 0, MISC_OFFSET_AB,
                      (long long)sizeof(*info), NULL, NULL);
}

/* ---- Public API ---- */

int get_current_slot(void)
{
    return device_get_current_slot();
}

/*
 * Mark the other slot as highest priority (for upgrade).
 * current_slot is passed for clarity but we re-read it from device.
 */
int set_slot_activity(struct storage_mgr *mgr, int current_slot)
{
    if (current_slot < 0 || current_slot > 1)
        return -1;

    struct avb_ab_data info;
    if (read_ab_misc(mgr, &info) != 0)
        return -1;

    if (avb_safe_memcmp(info.magic, AVB_AB_MAGIC, AVB_AB_MAGIC_LEN) != 0) {
        pr_err("A/B magic mismatch\n");
        return -1;
    }

    int other = 1 - current_slot;
    info.slots[current_slot].priority = AVB_AB_MAX_PRIORITY - 1;
    info.slots[other].priority = AVB_AB_MAX_PRIORITY;
    info.slots[other].tries_remaining = AVB_AB_MAX_TRIES_REMAINING;
    info.slots[other].successful_boot = 0;

    avb_ab_update_crc(&info);
    return write_ab_misc(mgr, &info);
}

/*
 * Mark current slot as successfully booted.
 * Called after OTA upgrade completes on the new slot.
 */
int set_slot_succeed(struct storage_mgr *mgr)
{
    int slot = get_current_slot();
    if (slot < 0)
        return -1;

    struct avb_ab_data info;
    if (read_ab_misc(mgr, &info) != 0)
        return -1;

    if (avb_safe_memcmp(info.magic, AVB_AB_MAGIC, AVB_AB_MAGIC_LEN) != 0) {
        pr_err("A/B magic mismatch\n");
        return -1;
    }

    if (info.slots[slot].priority != AVB_AB_MAX_PRIORITY) {
        pr_warn("slot %d priority %d != %d, correcting\n",
                slot, info.slots[slot].priority, AVB_AB_MAX_PRIORITY);
        info.slots[slot].priority = AVB_AB_MAX_PRIORITY;
    }

#ifdef SUCCESSFUL_BOOT
    info.slots[slot].tries_remaining = 0;
    info.slots[slot].successful_boot = 1;
#endif
#ifdef RETRY_BOOT
    info.slots[slot].tries_remaining = AVB_AB_MAX_TRIES_REMAINING;
    info.slots[slot].successful_boot = 0;
#endif
    info.last_boot = (uint8_t)slot;

    avb_ab_update_crc(&info);
    return write_ab_misc(mgr, &info);
}

/* Render a raw flash buffer for logging: printable ASCII as-is, NUL stops,
 * other bytes shown as \xNN. All-0xff (erased NAND) collapses to "<erased>".
 * dst_sz should be >= 4*max + 16. */
static void sanitize_str(char *dst, size_t dst_sz, const char *src, size_t max)
{
    size_t trimmed = max;
    while (trimmed > 0 && (unsigned char)src[trimmed - 1] == 0xff)
        trimmed--;
    if (trimmed == 0 && max > 0) {
        snprintf(dst, dst_sz, "<erased>");
        return;
    }

    size_t o = 0;
    for (size_t i = 0; i < max && o + 5 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\0')
            break;
        if (c >= 0x20 && c < 0x7f)
            dst[o++] = (char)c;
        else
            o += snprintf(dst + o, dst_sz - o, "\\x%02x", c);
    }
    dst[o < dst_sz ? o : dst_sz - 1] = '\0';
}

/* Display MISC partition contents for debugging */
void misc_display(struct storage_mgr *mgr)
{
    struct avb_ab_data info_ab;
    if (read_ab_misc(mgr, &info_ab) != 0) {
        pr_err("failed to read A/B data\n");
        return;
    }

    pr_info("=============== A/B Slot Info ===============\n");
    pr_info("magic: %c%c%c%c\n",
            info_ab.magic[0], info_ab.magic[1],
            info_ab.magic[2], info_ab.magic[3]);
    pr_info("version: %d.%d\n",
            info_ab.version_major, info_ab.version_minor);

    int cur = get_current_slot();
    for (int i = 0; i < AB_SLOT_NUM; i++) {
        pr_info("slot[%d]: priority=%d tries=%d successful=%d%s\n",
                i, info_ab.slots[i].priority,
                info_ab.slots[i].tries_remaining,
                info_ab.slots[i].successful_boot,
                (i == cur) ? " (current)" : "");
    }
    if (cur >= 0)
        pr_info("current_slot: %d (%c)\n", cur, 'a' + cur);
    else
        pr_info("current_slot: unknown\n");
    pr_info("last_boot: %d (recorded by --misc=now)\n", info_ab.last_boot);

    uint32_t calc = avb_htobe32(
        avb_crc32_calc((const uint8_t *)&info_ab,
                       sizeof(info_ab) - sizeof(uint32_t)));
    pr_info("crc32: stored=0x%08x calculated=0x%08x %s\n",
            info_ab.crc32, calc,
            info_ab.crc32 == calc ? "OK" : "MISMATCH");

    pr_info("=============== Wipe Command ================\n");
    char cmdline[32] = {0};
    struct storage_ops *ops = storage_resolve(mgr, MISC_PART_NAME);
    if (ops) {
        char path[256];
        if (storage_get_part_path(mgr, MISC_PART_NAME, path, sizeof(path)) == 0)
            ops->read(ops, path, cmdline, NULL, MISC_OFFSET_CMDLINE,
                      (long long)sizeof(cmdline) - 1);
    }
    char sbuf[3200];
    sanitize_str(sbuf, sizeof(sbuf), cmdline, sizeof(cmdline) - 1);
    pr_info("cmdline: %s\n", sbuf);

    pr_info("=============== BCB =========================\n");
    struct bootloader_message boot;
    if (bcb_read(mgr, &boot) == 0) {
        sanitize_str(sbuf, sizeof(sbuf), boot.command,  sizeof(boot.command));
        pr_info("command: %s\n", sbuf);
        sanitize_str(sbuf, sizeof(sbuf), boot.recovery, sizeof(boot.recovery));
        pr_info("recovery: %s\n", sbuf);
        sanitize_str(sbuf, sizeof(sbuf), boot.status,   sizeof(boot.status));
        pr_info("status: %s\n", sbuf);
    }
}

/* ---- A/B partition-name helpers (moved here from the deleted partition.c) ---- */

/* Return 'a' / 'b' for *_a / *_b names, else 0. */
char get_ab_suffix(const char *name)
{
    int len = (int)strlen(name);
    if (len >= 2 && name[len - 2] == '_') {
        char c = name[len - 1];
        if (c == 'a' || c == 'b')
            return c;
    }
    return 0;
}

/* True = SKIP this partition for the given slot context.
 *   current_slot < 0 (non-AB): keep unsuffixed, skip any _a/_b.
 *   current_slot 0/1 (AB):     keep unsuffixed (shared) + opposite-slot copy. */
bool slot_filter(char ab_suffix, int current_slot)
{
    if (current_slot < 0)
        return (ab_suffix != 0);
    if (ab_suffix == 0)
        return false;
    if (current_slot == 0 && ab_suffix == 'b')
        return false;   /* A active → update B */
    if (current_slot == 1 && ab_suffix == 'a')
        return false;   /* B active → update A */
    return true;        /* skip same-slot partitions */
}
