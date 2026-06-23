/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "image"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "recovery/image.h"
#include "recovery/config.h"
#include "recovery/log.h"
#include "recovery/checksum.h"

/* Adjust item offsets: add firmware offset to each item, set bootloader to boot area */
static void adjust_file_offsets(struct rk_image_hdr *hdr, uint32_t fw_offset,
                                uint32_t boot_offset, uint32_t boot_size)
{
    for (int i = 0; i < hdr->item_count; i++) {
        if (strcmp(hdr->item[i].name, "bootloader") == 0) {
            hdr->item[i].offset = boot_offset;
            hdr->item[i].size = boot_size;
            continue;
        }
        hdr->item[i].offset += fw_offset;
    }
}

static void display_hdr(const struct rk_image_hdr *hdr)
{
    pr_dbg("machine_model: %s\n", hdr->machine_model);
    pr_dbg("manufacturer: %s\n", hdr->manufacturer);
    pr_dbg("version: %u\n", hdr->version);
    pr_dbg("items: %d\n", hdr->item_count);

    for (int i = 0; i < hdr->item_count; i++) {
        const struct rk_image_item *item = &hdr->item[i];
        pr_info("  [%d] %-20s offset=0x%08x size=0x%08x flash_off=0x%08x\n",
                i, item->name, item->offset, item->size, item->flash_offset);
    }
}

/*
 * Parse a single RKFW image embedded at [base_offset, base_offset + fw_total)
 * within filepath. Used directly for plain RKFW files (base_offset = 0,
 * fw_total = file_size) and iteratively from analyze_ssfw for each sub-image.
 *
 * All item offsets in the resulting hdr are absolute file offsets (i.e.
 * already include base_offset), so callers downstream can read directly.
 */
static int analyze_rkfw(const char *filepath, long long base_offset,
                        long long fw_total, struct rk_image_hdr *hdr)
{
    struct rk_image_head head;
    uint8_t md5[32];

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", filepath, strerror(errno));
        return -1;
    }

    /* 1. Read image head at base_offset */
    if (lseek(fd, (off_t)base_offset, SEEK_SET) == (off_t)-1) {
        pr_err("seek to base 0x%llx\n", base_offset);
        close(fd);
        return -1;
    }
    if (read(fd, &head, sizeof(head)) != (ssize_t)sizeof(head)) {
        pr_err("read image head: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Validate STRUCT_RKIMAGE_HEAD.uiTag = RKFW (outer image header).
     * This is the tag at file offset 0 (or sub-image entry offset for SSFW). */
    if (head.tag != RKFW_TAG) {
        pr_err("STRUCT_RKIMAGE_HEAD.uiTag = 0x%x at base 0x%llx (expected RKFW 0x%x)\n",
               head.tag, base_offset, RKFW_TAG);
        close(fd);
        return -1;
    }

    /* Compute sub-image firmware-region size (>4GB support) */
    long long fw_region;
    if (head.reserved[14] == 'H' && head.reserved[15] == 'I') {
        uint32_t hi;
        memcpy(&hi, &head.reserved[16], sizeof(hi));
        fw_region = (long long)hi << 32;
        fw_region += head.fw_offset;
        fw_region += head.fw_size;
    } else {
        fw_region = (long long)head.fw_offset + head.fw_size;
    }
    head.fw_size = (uint32_t)(fw_region - head.fw_offset);

    pr_dbg("RKFW@0x%llx: tag=0x%x ver=0x%x boot_off=0x%x boot_sz=0x%x fw_off=0x%x fw_sz=0x%x\n",
           base_offset, head.tag, head.version,
           head.boot_offset, head.boot_size,
           head.fw_offset, head.fw_size);

    /* 2. Read MD5 trailer.
     *    Trailer sits at the very end of the sub-image (base + fw_total - 32),
     *    NOT at fw_region - 32. rkImageMaker aligns the file/sub-image so
     *    fw_total can exceed fw_region by padding; original analyticImage
     *    always uses fileSize/sub-image end. SSFW patch confirms: trailer at
     *    `offset + fw_size - 32`, MD5 over `[offset, offset + fw_size - 32)`.
     *    For signed images (fw_total > fw_region + 160), skip MD5. */
    long long md5_data_size = fw_total - fw_region;
    if (md5_data_size >= 160) {
        pr_warn("signed image (signature %lld bytes), skipping MD5\n",
                md5_data_size);
    } else {
        if (lseek(fd, (off_t)(base_offset + fw_total - 32), SEEK_SET) == (off_t)-1) {
            pr_err("seek to MD5 trailer\n");
            close(fd);
            return -1;
        }
        if (read(fd, md5, 32) != 32) {
            pr_err("read MD5 trailer\n");
            close(fd);
            return -1;
        }
    }

    /* 3. Read firmware header (RKIMAGE_HDR) at base_offset + head.fw_offset */
    if (lseek(fd, (off_t)(base_offset + head.fw_offset), SEEK_SET) == (off_t)-1) {
        pr_err("seek to fw_offset 0x%x (base 0x%llx)\n",
               head.fw_offset, base_offset);
        close(fd);
        return -1;
    }

    if (read(fd, hdr, sizeof(*hdr)) != (ssize_t)sizeof(*hdr)) {
        pr_err("read firmware header: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (hdr->tag != RKIMAGE_TAG) {
        pr_err("invalid fw header tag: 0x%x (expected 0x%x)\n",
               hdr->tag, RKIMAGE_TAG);
        close(fd);
        return -1;
    }

    /* Extended item count (>32 items, stored in manufacturer[56..59]) */
    if ((uint8_t)hdr->manufacturer[56] == 0x55 &&
        (uint8_t)hdr->manufacturer[57] == 0x66) {
        uint16_t extra;
        memcpy(&extra, &hdr->manufacturer[58], sizeof(extra));
        hdr->item_count += extra;
    }

    /* Adjust offsets to absolute file offsets (= base_offset + fw_offset + item.offset).
     * adjust_file_offsets handles the base + fw_offset addition; we pass base+fw
     * as the fw_offset argument to fold both adjustments into one pass. */
    adjust_file_offsets(hdr, (uint32_t)(base_offset + head.fw_offset),
                        (uint32_t)(base_offset + head.boot_offset),
                        head.boot_size);

    display_hdr(hdr);

    close(fd);

    /* 4. MD5 verification — covers [base_offset, base_offset + fw_total - 32) */
    if (md5_data_size < 160) {
        uint8_t digest[16];
        if (!md5_compute_file(filepath, base_offset, fw_total - 32, digest) ||
            !md5_verify_hex(md5, digest)) {
            pr_err("MD5 verification failed (base 0x%llx size %lld)\n",
                   base_offset, fw_total - 32);
            return -1;
        }
        pr_info("MD5 verification passed (base 0x%llx size %lld)\n",
                base_offset, fw_total - 32);
    }

    return 0;
}

/*
 * Parse SSFW merged image: iterate entry table, parse each embedded RKFW,
 * merge items into a single flat hdr. The entry.storage field is read for
 * the log but otherwise discarded — runtime probe handles routing.
 */
static int analyze_ssfw(const char *filepath, struct rk_image_hdr *hdr)
{
    struct ssfw_header ssfw_hdr;
    struct ssfw_entry  entries[MAX_SSFW_ENTRIES];

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", filepath, strerror(errno));
        return -1;
    }

    /* 1. SSFW header */
    if (read(fd, &ssfw_hdr, sizeof(ssfw_hdr)) != (ssize_t)sizeof(ssfw_hdr)) {
        pr_err("read SSFW header: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (ssfw_hdr.tag != SSFW_TAG) {
        pr_err("SSFW: bad tag 0x%x\n", ssfw_hdr.tag);
        close(fd);
        return -1;
    }
    if (ssfw_hdr.entry_count == 0 ||
        ssfw_hdr.entry_count > MAX_SSFW_ENTRIES) {
        pr_err("SSFW: bad entry_count %u\n", ssfw_hdr.entry_count);
        close(fd);
        return -1;
    }
    if (ssfw_hdr.entry_size != sizeof(struct ssfw_entry)) {
        pr_warn("SSFW: unexpected entry_size %u (expected %zu)\n",
                ssfw_hdr.entry_size, sizeof(struct ssfw_entry));
        /* tolerate — read entry_size bytes and ignore the trailing diff */
    }

    /* 2. Entry table */
    for (uint32_t i = 0; i < ssfw_hdr.entry_count; i++) {
        if (read(fd, &entries[i], sizeof(struct ssfw_entry)) !=
            (ssize_t)sizeof(struct ssfw_entry)) {
            pr_err("read SSFW entry %u\n", i);
            close(fd);
            return -1;
        }
        pr_info("SSFW entry %u: offset=0x%llx size=0x%llx storage=0x%x\n",
                i, (unsigned long long)entries[i].offset,
                (unsigned long long)entries[i].size, entries[i].storage);
    }
    close(fd);

    /* 3. Parse each sub-RKFW and merge items */
    memset(hdr, 0, sizeof(*hdr));
    int total = 0;

    for (uint32_t i = 0; i < ssfw_hdr.entry_count; i++) {
        struct rk_image_hdr sub;
        memset(&sub, 0, sizeof(sub));

        if (analyze_rkfw(filepath,
                         (long long)entries[i].offset,
                         (long long)entries[i].size,
                         &sub) != 0) {
            pr_err("SSFW: failed to parse sub-RKFW %u\n", i);
            return -1;
        }

        /* First sub-RKFW seeds the manifest metadata */
        if (i == 0) {
            hdr->tag     = sub.tag;
            hdr->size    = sub.size;
            hdr->version = sub.version;
            memcpy(hdr->machine_model, sub.machine_model, MAX_MACHINE_MODEL);
            memcpy(hdr->manufacturer,  sub.manufacturer,  MAX_MANUFACTURER);
        }

        /* Append sub-RKFW items. Each merged item carries its source
         * sub-RKFW's entry.storage in the parallel storage_code[] array so
         * resolve_for_full_flash routes to the right backend without
         * consulting by-name. */
        for (int j = 0; j < sub.item_count; j++) {
            /* package-file is per-RKFW manifest metadata, not a partition —
             * every sub-image has one. Skip it (build_upgrade_list skips it
             * downstream too; keeping it would collide across sub-images). */
            if (strcmp(sub.item[j].name, "package-file") == 0)
                continue;

            if (total >= MAX_PACKAGE_FILES) {
                pr_err("SSFW: too many items merged (>= %d)\n", MAX_PACKAGE_FILES);
                return -1;
            }

            /* Duplicate names across sub-images are legitimate in dual
             * storage — e.g. "parameter" exists on both NOR (its own param)
             * and the block disk (its GPT). storage_code[] disambiguates the
             * target backend, so do NOT reject the collision. */
            memcpy(&hdr->item[total], &sub.item[j], sizeof(struct rk_image_item));
            hdr->storage_code[total] = (uint16_t)entries[i].storage;
            total++;
        }
    }
    hdr->item_count = total;

    pr_info("SSFW: merged %d items from %u sub-image(s)\n",
            total, ssfw_hdr.entry_count);
    return 0;
}

int image_parse(const char *filepath, struct rk_image_hdr *hdr)
{
    /* Peek first 4 bytes for tag dispatch */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", filepath, strerror(errno));
        return -1;
    }

    uint32_t tag;
    if (read(fd, &tag, sizeof(tag)) != (ssize_t)sizeof(tag)) {
        pr_err("read leading tag: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    long long file_size = lseek(fd, 0, SEEK_END);
    close(fd);

    if (tag == SSFW_TAG) {
        pr_info("image_parse: SSFW (dual-storage merged)\n");
        return analyze_ssfw(filepath, hdr);
    } else if (tag == RKFW_TAG) {
        pr_info("image_parse: RKFW (single image)\n");
        return analyze_rkfw(filepath, 0, file_size, hdr);
    } else {
        pr_err("image_parse: unknown tag 0x%x (not RKFW 0x%x or SSFW 0x%x)\n",
               tag, RKFW_TAG, SSFW_TAG);
        return -1;
    }
}

const struct rk_image_item *image_find_item(const struct rk_image_hdr *hdr,
                                            const char *name)
{
    for (int i = 0; i < hdr->item_count && i < MAX_PACKAGE_FILES; i++) {
        if (strcmp(hdr->item[i].name, name) == 0)
            return &hdr->item[i];
    }
    return NULL;
}

bool image_get_version(const char *filepath, char *version, int max_len)
{
    struct rk_image_head head;

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", filepath, strerror(errno));
        return false;
    }

    if (read(fd, &head, sizeof(head)) != (ssize_t)sizeof(head)) {
        pr_err("read image head: %s\n", strerror(errno));
        close(fd);
        return false;
    }
    close(fd);

    unsigned major = (head.version >> 24) & 0xFF;
    unsigned minor = (head.version >> 16) & 0xFF;
    unsigned small = head.version & 0xFFFF;

    snprintf(version, max_len, "%u.%u.%u", major, minor, small);
    return true;
}
