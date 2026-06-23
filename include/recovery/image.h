/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_IMAGE_H
#define RK_RECOVERY_IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ---- RK image tags ----
 *
 * An update.img has TWO nested headers, each with its own fixed tag:
 *
 *   STRUCT_RKIMAGE_HEAD  at file offset 0
 *       .uiTag = RKFW_TAG (0x57464B52, "RKFW")
 *       .dwBootOffset / .dwBootSize / .dwFWOffset / .dwFWSize ...
 *
 *   RKIMAGE_HDR          at file offset = STRUCT_RKIMAGE_HEAD.dwFWOffset
 *       .tag = RKIMAGE_TAG (0x46414B52, "RKAF")
 *       .item[] (per-partition entries)
 *
 * Both tags are fixed, distinct constants — they are NOT two versions of
 * the same value. image_parse and analyze_rkfw validate each in turn.
 *
 * SSFW_TAG identifies the dual-storage merged wrapper format. When the file
 * starts with SSFW_TAG, the content is N stacked RKFW sub-images.
 */
#define RKFW_TAG            0x57464B52  /* "RKFW" — STRUCT_RKIMAGE_HEAD.uiTag */
#define RKIMAGE_TAG         0x46414B52  /* "RKAF" — RKIMAGE_HDR.tag (firmware section) */
#define SSFW_TAG            0x57465353  /* "SSFW" — dual-storage merged wrapper */
#define MAX_SSFW_ENTRIES    8

#define PART_NAME_LEN       32
#define RELATIVE_PATH_LEN   64
#define MAX_PACKAGE_FILES   32
#define MAX_MACHINE_MODEL   64
#define MAX_MANUFACTURER    60
#define IMAGE_RESERVED_SIZE 61

/* ---- RK timestamp ---- */
struct rk_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} __attribute__((packed));

/* ---- Image file header ---- */
struct rk_image_head {
    uint32_t        tag;            /* RKIMAGE_TAG */
    uint16_t        size;
    uint32_t        version;
    uint32_t        merge_version;
    struct rk_time  release_time;
    uint32_t        support_chip;
    uint32_t        boot_offset;
    uint32_t        boot_size;
    uint32_t        fw_offset;
    uint32_t        fw_size;
    uint8_t         reserved[IMAGE_RESERVED_SIZE];
} __attribute__((packed));

/* ---- Image item (partition entry in image) ---- */
struct rk_image_item {
    char        name[PART_NAME_LEN];
    char        file[RELATIVE_PATH_LEN];
    uint32_t    offset;
    uint32_t    flash_offset;
    uint32_t    usespace;
    uint32_t    size;
} __attribute__((packed));

/* ---- Image header (package manifest) ---- */
struct rk_image_hdr {
    uint32_t    tag;
    uint32_t    size;
    char        machine_model[MAX_MACHINE_MODEL];
    char        manufacturer[MAX_MANUFACTURER];
    uint32_t    version;
    int32_t     item_count;
    struct rk_image_item item[MAX_PACKAGE_FILES];

    /*
     * Per-item storage routing tag — parallel array indexed by item[].
     *
     * SSFW images: filled by analyze_ssfw from each sub-RKFW's
     * ssfw_entry.storage (vendor STORAGE_*_CODE bitmask). Used by
     * resolve_for_full_flash to route each item to the right backend
     * without consulting by-name softlinks (which don't exist on a
     * virgin board being flashed from SD/USB).
     *
     * Plain RKFW images: left as 0. resolve_for_full_flash falls back
     * to mgr->backends[0] when backend_count == 1, errors when > 1.
     *
     * Parallel array (not a field inside rk_image_item) mirrors the
     * legacy dual-storage patch's image_storage[] design — keeps the
     * on-wire RKIMAGE_ITEM layout untouched and lets OTA paths ignore
     * this field entirely.
     */
    uint16_t    storage_code[MAX_PACKAGE_FILES];
};

/* ---- SSFW (dual-storage merged image) header / entry ----
 * The merged image produced by `rkImageMaker -merge` wraps N sub-RKFWs:
 *   [ssfw_header][ssfw_entry × N][RKFW image 0][RKFW image 1]...
 * Each entry tells where its embedded RKFW lives in the file.
 *
 * The `storage` field carries a vendor STORAGE_*_CODE bitmask that
 * identifies which physical medium this sub-RKFW targets. analyze_ssfw
 * propagates each entry.storage to rk_image_hdr.storage_code[] for the
 * items it merges in, so SD/USB full-flash mode can route partitions to
 * the right backend on a virgin board (no by-name softlinks exist yet).
 * OTA mode ignores storage_code — by-name takes precedence.
 */
struct ssfw_header {
    uint32_t tag;              /* SSFW_TAG */
    uint32_t head_size;
    uint32_t entry_size;
    uint32_t version;
    uint32_t entry_count;
    uint8_t  reserved[44];
} __attribute__((packed));

struct ssfw_entry {
    uint64_t offset;           /* byte offset of sub-RKFW in this file */
    uint64_t size;             /* byte size of sub-RKFW */
    uint32_t storage;          /* STORAGE_*_CODE — propagated into rk_image_hdr.storage_code[] */
    uint32_t reserved[3];
    char     file[32];
} __attribute__((packed));

/*
 * >4GB image compatibility:
 * Original code reuses item->file[50]='H' and file[55]='H' to indicate
 * offset/size exceeding 4GB. file[51-54] = high offset, file[56-59] = high size.
 * These helpers encapsulate that logic.
 */
static inline long long image_get_offset(const struct rk_image_item *item)
{
    long long off = item->offset;
    if (item->file[50] == 'H') {
        uint32_t hi;
        __builtin_memcpy(&hi, &item->file[51], sizeof(hi));
        off |= (long long)hi << 32;
    }
    return off;
}

static inline long long image_get_size(const struct rk_image_item *item)
{
    long long sz = item->size;
    if (item->file[55] == 'H') {
        uint32_t hi;
        __builtin_memcpy(&hi, &item->file[56], sizeof(hi));
        sz |= (long long)hi << 32;
    }
    return sz;
}

/* Parse firmware image, populate hdr */
int image_parse(const char *filepath, struct rk_image_hdr *hdr);

/* Find item by partition name, returns NULL if not found */
const struct rk_image_item *image_find_item(const struct rk_image_hdr *hdr,
                                            const char *name);

/* Get image version string */
bool image_get_version(const char *filepath, char *version, int max_len);

#endif /* RK_RECOVERY_IMAGE_H */
