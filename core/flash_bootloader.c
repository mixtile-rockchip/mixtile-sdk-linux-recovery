/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "flash:bootloader"

#include "recovery/config.h"

#if defined(RK_IDBLOCK_UPDATE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>

#include "recovery/storage.h"
#include "recovery/log.h"
#include "recovery/config.h"
#include "recovery/checksum.h"
#include "recovery/flash.h"
#include "recovery/device.h"
#include "flash_internal.h"

/* ---- Constants ---- */
#define RKBOOT_TAG_OLD      0x544F4F42  /* "BOOT" */
#define RKBOOT_TAG_NEW      0x2052444C  /* "LDR " */
#define IDB_TAG             0x0FF0AA55
#define PAGEALIGN(x)        (((x) > 0) ? (((x) - 1) / 4 + 1) : (x))

/* ---- RKBOOT structures (packed, from rkimage.h) ---- */

#pragma pack(1)

struct rk_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

struct rkboot_head {
    uint32_t        tag;
    uint16_t        size;
    uint32_t        version;
    uint32_t        merge_version;
    struct rk_time  release_time;
    uint32_t        support_chip;
    uint8_t         entry_471_count;
    uint32_t        entry_471_offset;
    uint8_t         entry_471_size;
    uint8_t         entry_472_count;
    uint32_t        entry_472_offset;
    uint8_t         entry_472_size;
    uint8_t         loader_entry_count;
    uint32_t        loader_entry_offset;
    uint8_t         loader_entry_size;
    uint8_t         sign_flag;
    uint8_t         rc4_flag;
    uint8_t         reserved[57];
};

struct rkboot_entry {
    uint8_t   size;
    uint32_t  type;
    uint16_t  name[20];
    uint32_t  data_offset;
    uint32_t  data_size;
    uint32_t  data_delay;
};

/* IDBlock sector structures */
struct idb_sec0 {
    uint32_t tag;
    uint8_t  reserved[4];
    uint32_t rc4_flag;
    uint16_t boot_code1_offset;
    uint16_t boot_code2_offset;
    uint8_t  reserved1[490];
    uint16_t boot_data_size;
    uint16_t boot_code_size;
    uint16_t crc;
};

struct idb_sec1 {
    uint16_t sys_reserved_block;
    uint16_t disk0_size;
    uint16_t disk1_size;
    uint16_t disk2_size;
    uint16_t disk3_size;
    uint32_t chip_tag;
    uint32_t machine_id;
    uint16_t loader_year;
    uint16_t loader_date;
    uint16_t loader_ver;
    uint16_t last_loader_ver;
    uint16_t read_write_times;
    uint32_t fw_ver;
    uint16_t machine_info_len;
    uint8_t  machine_info[30];
    uint16_t factory_info_len;
    uint8_t  factory_info[30];
    uint16_t flash_info_offset;
    uint16_t flash_info_len;
    uint8_t  reserved[384];
    uint32_t flash_size;
    uint8_t  reserved1;
    uint8_t  access_time;
    uint16_t block_size;
    uint8_t  page_size;
    uint8_t  ecc_bits;
    uint8_t  reserved2[8];
    uint16_t idblock0;
    uint16_t idblock1;
    uint16_t idblock2;
    uint16_t idblock3;
    uint16_t idblock4;
};

#define CHIPINFO_LEN            16
#define IDB_SEC2_RESERVED_LEN   473

struct idb_sec2 {
    uint16_t info_size;
    uint8_t  chip_info[CHIPINFO_LEN];
    uint8_t  reserved[IDB_SEC2_RESERVED_LEN];
    char     vc_tag[3];
    uint16_t sec0_crc;
    uint16_t sec1_crc;
    uint32_t boot_code_crc;
    uint16_t sec3_custom_data_offset;
    uint16_t sec3_custom_data_size;
    char     crc_tag[4];
    uint16_t sec3_crc;
};

#pragma pack()

/*
 * Per-call state for a single IDBlock parse + generate. Previously held in
 * file-static globals, which made rkboot.c non-reentrant — a problem for
 * dual-storage where Nor and eMMC each have their own loader to process.
 * Now lives entirely on the stack of download_loader().
 */
struct rkboot_session {
    uint16_t flash_data_sec;
    uint16_t flash_boot_sec;
    uint16_t flash_head_sec;
    uint32_t loader_size;
    uint32_t loader_data_size;
    uint32_t sec_num_per_idb;
    uint16_t phy_block_per_idb;
    bool     rc4_disable;
    bool     new_idb_flag;
    uint32_t idblock_offset[IDB_BLOCKS];
    long long flash_total_size;

    const struct rkboot_head *boot_head;
};

/* ---- Helpers ---- */

static uint16_t ushort_to_bcd(uint16_t num)
{
    return (num % 10) | (((num / 10) % 10) << 4) |
           (((num / 100) % 10) << 8) | (((num / 1000) % 10) << 12);
}

static uint8_t byte_to_bcd(uint8_t num)
{
    return (num % 10) | (((num / 10) % 10) << 4);
}

static void wchar_to_char(const uint16_t *src, char *dst, int len)
{
    memset(dst, 0, len);
    for (int i = 0; i < len; i++) {
        dst[i] = (char)(src[i] & 0xFF);
    }
}

static uint32_t get_loader_data(const struct rkboot_session *s,
                                const char *name, const uint8_t *data_buf,
                                uint32_t buf_size, uint8_t **out_buf)
{
    *out_buf = NULL;

    uint32_t offset = s->boot_head->loader_entry_offset;
    uint8_t count = s->boot_head->loader_entry_count;
    uint8_t esize = s->boot_head->loader_entry_size;

    if (esize < sizeof(struct rkboot_entry)) {
        pr_err("rkboot: bad entry size %u\n", esize);
        return 0;
    }

    /* Entry table must fit entirely within data_buf */
    uint64_t table_end = (uint64_t)offset + (uint64_t)esize * count;
    if (offset > buf_size || table_end > buf_size) {
        pr_err("rkboot: entry table out of range (off=%u esize=%u cnt=%u buf=%u)\n",
               offset, esize, count, buf_size);
        return 0;
    }

    for (uint8_t i = 0; i < count; i++) {
        const struct rkboot_entry *ent =
            (const struct rkboot_entry *)(data_buf + offset + (uint32_t)esize * i);
        char sz_name[20];
        wchar_to_char(ent->name, sz_name, 20);
        if (strcmp(name, sz_name) == 0) {
            uint32_t sz = ent->data_size;
            uint64_t end = (uint64_t)ent->data_offset + (uint64_t)sz;
            if (sz == 0 || sz > RK_LOADER_MAX_DATA ||
                ent->data_offset > buf_size || end > buf_size) {
                pr_err("rkboot: entry '%s' bad range (off=%u sz=%u buf=%u)\n",
                       sz_name, ent->data_offset, sz, buf_size);
                return 0;
            }
            *out_buf = malloc(sz);
            if (!*out_buf)
                return 0;
            memset(*out_buf, 0, sz);
            memcpy(*out_buf, data_buf + ent->data_offset, sz);
            return sz;
        }
    }
    return 0;
}

static int get_loader_entry_index(const struct rkboot_session *s,
                                  const uint8_t *data_buf, const char *name)
{
    uint32_t offset = s->boot_head->loader_entry_offset;
    uint8_t count = s->boot_head->loader_entry_count;
    uint8_t esize = s->boot_head->loader_entry_size;

    for (uint8_t i = 0; i < count; i++) {
        const struct rkboot_entry *ent =
            (const struct rkboot_entry *)(data_buf + offset + esize * i);
        char sz_name[20];
        wchar_to_char(ent->name, sz_name, 20);
        if (strcmp(name, sz_name) == 0)
            return (int)i;
    }
    return -1;
}

static int find_valid_blocks(int begin, int len, const uint8_t *block_state)
{
    int count = 0;
    int index = begin;
    while (begin < IDBLOCK_TOP) {
        if (block_state[begin++] == 0)
            ++count;
        else {
            count = 0;
            index = begin;
        }
        if (count >= len)
            break;
    }
    return (begin >= IDBLOCK_TOP) ? -1 : index;
}

/* ---- Sector builders (old format) ---- */

static void make_sector0(const struct rkboot_session *s, uint8_t *sector)
{
    struct idb_sec0 *out = (struct idb_sec0 *)sector;
    memset(sector, 0, SECTOR_SIZE);
    out->tag = IDB_TAG;
    if (s->rc4_disable)
        out->rc4_flag = 1;
    out->boot_code1_offset = 0x4;
    out->boot_code2_offset = 0x4;
    out->boot_data_size = s->flash_data_sec;
    out->boot_code_size = s->flash_data_sec + s->flash_boot_sec;
}

static void make_sector1(const struct rkboot_session *s, uint8_t *sector)
{
    struct idb_sec1 *out = (struct idb_sec1 *)sector;
    memset(sector, 0, SECTOR_SIZE);

    uint16_t sys_reserved;
    if ((s->idblock_offset[4] + 1) % 12 == 0)
        sys_reserved = s->idblock_offset[4] + 13;
    else
        sys_reserved = ((s->idblock_offset[4] + 1) / 12 + 1) * 12;
    if (sys_reserved > IDBLOCK_TOP)
        sys_reserved = IDBLOCK_TOP;
    out->sys_reserved_block = sys_reserved;

    out->chip_tag = 0x38324B52;
    out->loader_year = ushort_to_bcd(s->boot_head->release_time.year);
    out->loader_date = (uint16_t)byte_to_bcd(s->boot_head->release_time.month) << 8 |
                       byte_to_bcd(s->boot_head->release_time.day);
    out->loader_ver = (uint16_t)s->boot_head->version;
    out->read_write_times = 1;
    out->flash_size = (uint32_t)(s->flash_total_size * 1024);

    out->idblock0 = s->idblock_offset[0];
    out->idblock1 = s->idblock_offset[1];
    out->idblock2 = s->idblock_offset[2];
    out->idblock3 = s->idblock_offset[3];
    out->idblock4 = s->idblock_offset[4];
}

static void make_sector2(uint8_t *sector)
{
    struct idb_sec2 *out = (struct idb_sec2 *)sector;
    memset(sector, 0, SECTOR_SIZE);
    out->info_size = 0;
    memcpy(out->vc_tag, "VC", 2);
    memcpy(out->crc_tag, "CRC", 3);
}

/* ---- IDBlock data generation ---- */

static int make_idblock_data(struct rkboot_session *s, uint8_t *idb,
                             uint8_t *loader_code, uint8_t *loader_data)
{
    uint8_t sec0[SECTOR_SIZE], sec1[SECTOR_SIZE], sec2[SECTOR_SIZE], sec3[SECTOR_SIZE];

    make_sector0(s, sec0);
    make_sector1(s, sec1);
    make_sector2(sec2);
    memset(sec3, 0, SECTOR_SIZE);

    struct idb_sec2 *s2 = (struct idb_sec2 *)sec2;
    s2->sec0_crc = rk_crc16(sec0, SECTOR_SIZE);
    s2->sec1_crc = rk_crc16(sec1, SECTOR_SIZE);
    s2->sec3_crc = rk_crc16(sec3, SECTOR_SIZE);

    memcpy(idb, sec0, SECTOR_SIZE);
    memcpy(idb + SECTOR_SIZE, sec1, SECTOR_SIZE);
    memcpy(idb + SECTOR_SIZE * 3, sec3, SECTOR_SIZE);

    /* RC4 encryption of loader data/code */
    struct idb_sec0 *s0 = (struct idb_sec0 *)sec0;
    if (s0->rc4_flag) {
        for (uint32_t i = 0; i < s->loader_data_size / SECTOR_SIZE; i++)
            rk_rc4(loader_data + SECTOR_SIZE * i, SECTOR_SIZE);
        for (uint32_t i = 0; i < s->loader_size / SECTOR_SIZE; i++)
            rk_rc4(loader_code + SECTOR_SIZE * i, SECTOR_SIZE);
    }

    memcpy(idb + SECTOR_SIZE * 4, loader_data, s->loader_data_size);
    memcpy(idb + SECTOR_SIZE * (4 + s->flash_data_sec), loader_code, s->loader_size);

    s2->boot_code_crc = rk_crc32(idb + SECTOR_SIZE * 4,
                                  s0->boot_code_size * SECTOR_SIZE, 0);
    memcpy(idb + SECTOR_SIZE * 2, sec2, SECTOR_SIZE);

    /* RC4 encrypt sectors 0, 2, 3 (skip 1) */
    for (int i = 0; i < 4; i++) {
        if (i != 1)
            rk_rc4(idb + SECTOR_SIZE * i, SECTOR_SIZE);
    }

    return 0;
}

static int make_new_idblock_data(const struct rkboot_session *s, uint8_t *idb,
                                 const uint8_t *data_buf)
{
    uint32_t offset;
    int head_index = get_loader_entry_index(s, data_buf, "FlashHead");
    if (head_index < 0) {
        pr_err("FlashHead entry not found\n");
        return -1;
    }

    uint32_t entry_offset = s->boot_head->loader_entry_offset;
    uint8_t entry_count = s->boot_head->loader_entry_count;
    uint8_t entry_size = s->boot_head->loader_entry_size;

    /* Process FlashHead first */
    const struct rkboot_entry *ent =
        (const struct rkboot_entry *)(data_buf + entry_offset + entry_size * head_index);
    uint32_t entry_sz = PAGEALIGN(BYTE2SECTOR(ent->data_size)) * 4 * SECTOR_SIZE;
    uint8_t *buf = malloc(entry_sz);
    if (!buf)
        return -1;

    memset(buf, 0, entry_sz);
    memcpy(buf, data_buf + ent->data_offset, ent->data_size);

    if (s->rc4_disable) {
        for (uint32_t j = 0; j < entry_sz / SECTOR_SIZE; j++)
            rk_rc4(buf + SECTOR_SIZE * j, SECTOR_SIZE);
    }
    memcpy(idb, buf, entry_sz);
    offset = entry_sz;

    /* Process remaining entries */
    for (uint8_t i = 0; i < entry_count; i++) {
        if ((int)i == head_index)
            continue;

        free(buf);
        ent = (const struct rkboot_entry *)(data_buf + entry_offset + entry_size * i);
        entry_sz = PAGEALIGN(BYTE2SECTOR(ent->data_size)) * 4 * SECTOR_SIZE;
        buf = malloc(entry_sz);
        if (!buf)
            return -1;

        memset(buf, 0, entry_sz);
        memcpy(buf, data_buf + ent->data_offset, ent->data_size);

        if (s->rc4_disable) {
            for (uint32_t j = 0; j < entry_sz / SECTOR_SIZE; j++)
                rk_rc4(buf + SECTOR_SIZE * j, SECTOR_SIZE);
        }
        memcpy(idb + offset, buf, entry_sz);
        offset += entry_sz;
    }

    free(buf);
    return 0;
}

static void calc_idb_count(struct rkboot_session *s)
{
    if (!s->new_idb_flag)
        s->sec_num_per_idb = 4 + s->flash_data_sec + s->flash_boot_sec;
    else
        s->sec_num_per_idb = s->flash_head_sec;

    s->phy_block_per_idb = (s->sec_num_per_idb > 0)
        ? (uint16_t)((s->sec_num_per_idb - 1) / 8 + 1) : 0;
    pr_dbg("phy_block_per_idb = %u\n", s->phy_block_per_idb);
}

static int reserve_idblock(struct rkboot_session *s)
{
    uint8_t block_state[IDBLOCK_TOP];
    memset(s->idblock_offset, 0, sizeof(s->idblock_offset));
    memset(block_state, 0, sizeof(block_state));

    int index = 0;
    for (int i = 0; i < IDB_BLOCKS; i++) {
        int ret = find_valid_blocks(index, s->phy_block_per_idb, block_state);
        if (ret < 0) {
            pr_err("cannot find valid blocks for IDB copy %d\n", i);
            return -1;
        }
        s->idblock_offset[i] = (uint32_t)ret;
        index = ret + s->phy_block_per_idb;
    }
    return 0;
}

/* ---- IDBlock copy geometry + flatten (payload placement) ---- */

/*
 * Per-medium IDBlock copy geometry — where the bootrom reads the IDBlock, how
 * many redundant copies, and the per-copy slot size. These are fixed bootrom /
 * vendor conventions verified against rk3506 + u-boot, EXCEPT the NAND base,
 * which is one erase block (a runtime property of the chip — see
 * idblock_geom_for). Kept here next to the IDBlock generation rather than as a
 * storage_ops method: placing a payload is the payload's concern, not a storage
 * mechanism (storage just writes bytes; GPT works the same way).
 */
struct idblock_geom {
    uint32_t base;          /* device byte offset of copy 0 */
    uint32_t granularity;   /* per-copy slot = align_up(data_size, this) */
    int      copies;        /* redundant copies */
};

#define NAND_IDB_GRAN   (256 * 1024)  /* rk3506 WriteIDBlock 256KB slot (fixed) */
#define NOR_IDB_BASE    (64 * 1024)   /* vendor doc: SPI Nor idb @64KB, dual, 64KB-aligned */
#define NOR_IDB_GRAN    (64 * 1024)
#define EMMC_IDB_BASE   0x8000        /* 32KB; rk3506 "32KB + 512KB*n" */
#define EMMC_IDB_GRAN   (512 * 1024)

/* The erase block size (bytes) of an MTD device — the NAND IDBlock base. */
static uint32_t mtd_erase_block_size(const char *dev)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        pr_err("idb: open %s: %s\n", dev, strerror(errno));
        return 0;
    }
    mtd_info_t info;
    int rc = ioctl(fd, MEMGETINFO, &info);
    close(fd);
    if (rc < 0) {
        pr_err("idb: MEMGETINFO %s: %s\n", dev, strerror(errno));
        return 0;
    }
    return info.erasesize;
}

/*
 * Resolve the IDBlock copy geometry for a backend. Returns false when the
 * medium hosts no IDBlock (e.g. SATA) — caller graceful-skips. NAND base is
 * the erase block size (rk3506: bootrom reads NAND IDBlock at one erase block);
 * all other values are fixed conventions.
 */
static bool idblock_geom_for(struct storage_ops *ops, const char *dev,
                             struct idblock_geom *g)
{
    switch (ops->type) {
    case STORAGE_NAND:
        g->base = mtd_erase_block_size(dev);
        if (g->base == 0)
            return false;
        g->granularity = NAND_IDB_GRAN;
        g->copies = IDB_BLOCKS;
        return true;
    case STORAGE_NOR:
        g->base = NOR_IDB_BASE;
        g->granularity = NOR_IDB_GRAN;
        g->copies = 2;
        return true;
    case STORAGE_EMMC:
    case STORAGE_UFS:
        g->base = EMMC_IDB_BASE;
        g->granularity = EMMC_IDB_GRAN;
        g->copies = IDB_BLOCKS;
        return true;
    default:                /* SATA and any non-bootable medium */
        return false;
    }
}

/*
 * Lay out `g.copies` copies of `idb` into one flat buffer, each on its own slot
 * of align_up(data_size, g.granularity) bytes (gaps zero-padded). Returns the
 * buffer (caller frees) and its length, or NULL on OOM. This is the rk3506
 * WriteIDBlock model: a single directly-writable buffer the storage backend
 * writes in one go (NAND nandwrite -p then skips bad blocks).
 */
static uint8_t *idblock_flatten(const uint8_t *idb, size_t data_size,
                                struct idblock_geom g, size_t *out_len)
{
    size_t slot = (data_size + g.granularity - 1) / g.granularity * g.granularity;
    if (slot == 0)
        slot = g.granularity;
    size_t len = slot * (size_t)g.copies;

    uint8_t *flat = calloc(1, len);
    if (!flat) {
        pr_err("idb: flatten oom %zu\n", len);
        return NULL;
    }
    for (int i = 0; i < g.copies; i++)
        memcpy(flat + (size_t)i * slot, idb, data_size);

    *out_len = len;
    return flat;
}

/* ---- Public API ---- */

static bool download_loader(const uint8_t *data_buf, uint32_t size,
                            struct storage_ops *bl_ops, const char *bl_dev)
{
    if (!data_buf || size < sizeof(struct rkboot_head)) {
        pr_err("rkboot: buffer too small (%u)\n", size);
        return false;
    }

    /* No backend / target → nothing owns the IDBlock here. Graceful skip
     * (e.g. OTA board without an idb/loader partition exposed). */
    if (!bl_ops || !bl_dev || !bl_dev[0]) {
        pr_info("IDBlock skipped — no bootloader target\n");
        return true;
    }

    struct rkboot_session s = {0};
    s.boot_head = (const struct rkboot_head *)data_buf;

    if (s.boot_head->tag != RKBOOT_TAG_OLD && s.boot_head->tag != RKBOOT_TAG_NEW) {
        pr_err("invalid boot tag: 0x%08x\n", s.boot_head->tag);
        return false;
    }

    s.new_idb_flag = (s.boot_head->tag == RKBOOT_TAG_NEW);
    s.rc4_disable = (s.boot_head->rc4_flag != 0);

    if (s.new_idb_flag)
        pr_info("new IDB format (LDR)\n");

    /* Flash size (KiB) for sector1.flash_size — informational. disk_lbas
     * gives the (LB count, LB size) pair; count × lb / 1024 → KiB. (NAND
     * excludes its trailing BBT reserve, immaterial for this field.) */
    uint32_t lb = 0;
    long long lbas = bl_ops->disk_lbas(bl_ops, bl_dev, &lb);
    s.flash_total_size = (lbas > 0) ? lbas * lb / 1024 : 0;

    /* Parse loader entries */
    uint8_t *loader_data_buf = NULL;
    s.loader_data_size = get_loader_data(&s, "FlashData", data_buf, size, &loader_data_buf);
    s.flash_data_sec = PAGEALIGN(BYTE2SECTOR(s.loader_data_size)) * 4;

    uint8_t *loader_code_buf = NULL;
    s.loader_size = get_loader_data(&s, "FlashBoot", data_buf, size, &loader_code_buf);
    s.flash_boot_sec = PAGEALIGN(BYTE2SECTOR(s.loader_size)) * 4;

    pr_dbg("flash_data_sec=%u flash_boot_sec=%u\n", s.flash_data_sec, s.flash_boot_sec);

    if (s.new_idb_flag) {
        s.flash_head_sec = 0;
        uint8_t ec = s.boot_head->loader_entry_count;
        uint32_t entry_off = s.boot_head->loader_entry_offset;
        uint8_t  esize = s.boot_head->loader_entry_size;
        uint64_t table_end = (uint64_t)entry_off + (uint64_t)esize * ec;
        if (esize < sizeof(struct rkboot_entry) ||
            entry_off > size || table_end > size) {
            pr_err("rkboot: entry table out of range in new_idb path\n");
            free(loader_data_buf);
            free(loader_code_buf);
            return false;
        }
        for (uint8_t i = 0; i < ec; i++) {
            const struct rkboot_entry *ent =
                (const struct rkboot_entry *)(data_buf + entry_off +
                                              (uint32_t)esize * i);
            if (ent->data_size > RK_LOADER_MAX_DATA) {
                pr_err("rkboot: entry %u oversized data_size=%u\n",
                       i, ent->data_size);
                free(loader_data_buf);
                free(loader_code_buf);
                return false;
            }
            s.flash_head_sec += PAGEALIGN(BYTE2SECTOR(ent->data_size)) * 4;
        }
    }

    calc_idb_count(&s);
    /*
     * reserve_idblock() computes a NAND bad-block-aware block map into
     * s.idblock_offset[], which only the OLD idb format consumes (see
     * make_idblock_data). The NEW (LDR) format doesn't use it, and on NOR
     * the bad-block search (MEMGETBADBLOCK) can't run — so skip it there to
     * avoid the spurious "cannot find valid blocks" error. The write path
     * places copies at the geometry's fixed slots regardless.
     */
    if (!s.new_idb_flag)
        reserve_idblock(&s);

    /* IDBlock copy geometry for this medium (NAND base = erase block, runtime). */
    struct idblock_geom geom;
    if (!idblock_geom_for(bl_ops, bl_dev, &geom)) {
        pr_info("IDBlock skipped — %s hosts no IDBlock\n", bl_ops->name);
        return true;
    }

    /* Generate IDBlock data */
    uint8_t *idb_data = calloc(1, s.sec_num_per_idb * SECTOR_SIZE);
    if (!idb_data) {
        free(loader_data_buf);
        free(loader_code_buf);
        return false;
    }

    int ret;
    if (s.new_idb_flag) {
        ret = make_new_idblock_data(&s, idb_data, data_buf);
    } else {
        ret = make_idblock_data(&s, idb_data, loader_code_buf, loader_data_buf);
    }

    if (ret != 0) {
        pr_err("make IDBlock data failed\n");
        free(idb_data);
        free(loader_data_buf);
        free(loader_code_buf);
        return false;
    }

    /* Lay the copies out into one flat buffer, then write it via the backend's
     * generic write (NAND: nandwrite -p skips bad blocks; NOR/block: plain). */
    size_t flat_len = 0;
    uint8_t *flat = idblock_flatten(idb_data, s.sec_num_per_idb * SECTOR_SIZE,
                                    geom, &flat_len);
    if (!flat) {
        free(idb_data);
        free(loader_data_buf);
        free(loader_code_buf);
        return false;
    }

    pr_info("writing IDBlock to %s @0x%x: %d copies, slot=%uKiB, flat=%zu\n",
            bl_dev, geom.base, geom.copies, geom.granularity >> 10, flat_len);

    int wr = bl_ops->write(bl_ops, bl_dev, flat, NULL, 0,
                           (off_t)geom.base, (long long)flat_len, NULL, NULL);
    free(flat);
    if (wr != 0) {
        pr_err("write IDBlock failed\n");
        free(idb_data);
        free(loader_data_buf);
        free(loader_code_buf);
        return false;
    }

    pr_info("IDBlock written successfully (%u sectors x %d copies)\n",
            s.sec_num_per_idb, geom.copies);
    free(idb_data);
    free(loader_data_buf);
    free(loader_code_buf);
    return true;
}

/*
 * Engine entry: flash the bootloader (IDBlock) item. Resolves the target
 * device, reads the bootloader image, and hands off to download_loader, which
 * generates the IDBlock, lays out the redundant copies, and writes them.
 */
int flash_bootloader(struct upgrade_item *item, const char *image_path)
{
    pr_info("flash_bootloader: %s (size=%lld)\n", item->name, item->img_size);

    /* Graceful skip if backend is missing (partition not present on this
     * device — e.g. dts未启用 sfc_nor 时 Nor 不可见). build_upgrade_list
     * already filters NULL-backend items, but be defensive. */
    if (!item->backend) {
        pr_info("flash_bootloader: no backend, skip\n");
        return 0;
    }

    if (item->img_size <= 0 || item->img_size > 16 * 1024 * 1024) {
        pr_err("invalid bootloader size: %lld\n", item->img_size);
        return -1;
    }

    /* Target resolved by the engine (resolve_disk_level, both modes): the
     * backend's whole-device IDBlock node (MTD master / eMMC whole device /
     * UFS boot LUN), which geom.base addresses absolutely. The engine
     * aborts the upgrade when no such node exists — no fallback here. */
    const char *bl_dev = item->dev_path;
    if (!bl_dev[0]) {
        pr_err("flash_bootloader: no IDBlock target device\n");
        return -1;
    }

    /* Read bootloader data from image */
    uint8_t *data_buf = malloc((size_t)item->img_size);
    if (!data_buf) {
        pr_err("malloc %lld bytes failed\n", item->img_size);
        return -1;
    }

    int fd = open(image_path, O_RDONLY);
    if (fd < 0) {
        pr_err("open %s: %s\n", image_path, strerror(errno));
        free(data_buf);
        return -1;
    }

    if (lseek(fd, item->img_offset, SEEK_SET) == (off_t)-1) {
        pr_err("lseek to 0x%llx: %s\n", item->img_offset, strerror(errno));
        close(fd);
        free(data_buf);
        return -1;
    }

    ssize_t nread = read(fd, data_buf, (size_t)item->img_size);
    close(fd);

    if (nread != (ssize_t)item->img_size) {
        pr_err("read bootloader data: %s\n", strerror(errno));
        free(data_buf);
        return -1;
    }

    bool ok = download_loader(data_buf, (uint32_t)item->img_size,
                              item->backend, bl_dev);
    free(data_buf);

    if (!ok) {
        pr_err("download_loader failed\n");
        return -1;
    }

    pr_info("bootloader flash complete\n");
    return 0;
}

#endif /* RK_IDBLOCK_UPDATE */
