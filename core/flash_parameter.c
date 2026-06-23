/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 * GPT (parameter) payload — the complete chain in one file, mirroring
 * flash_bootloader.c (IDBlock): parameter text parsing → primary+backup GPT
 * generation (gpt_build lays both out in one directly-writable buffer) →
 * backend->write. Storage backends only supply device facts — the live
 * logical block size (512B media vs 4KB UFS picks the GPT geometry; this is
 * what fixed u-boot's "Invalid GPT" on SATA/NOR) and the safe disk size in
 * sectors (excludes the NAND BBT tail; the backup GPT lands at its end).
 */

#define __MODULE__ "flash:parameter"

#include "recovery/config.h"

#if defined(RK_GPT_UPDATE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#include "recovery/flash.h"
#include "recovery/parameter.h"
#include "recovery/log.h"

/* ============================================================
 * GPT on-disk format (UEFI spec subset; little-endian target)
 * ============================================================ */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define cpu_to_le16(x)  (x)
#define cpu_to_le32(x)  (x)
#define cpu_to_le64(x)  (x)
#define le16_to_cpu(x)  (x)
#define le32_to_cpu(x)  (x)
#define le64_to_cpu(x)  (x)

#define uswap_16(x) \
    ((((x) & 0xff00) >> 8) | (((x) & 0x00ff) << 8))
#define uswap_32(x) \
    ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >> 8) | \
     (((x) & 0x0000ff00) << 8)  | (((x) & 0x000000ff) << 24))

#define cpu_to_be16(x)  uswap_16(x)
#define cpu_to_be32(x)  uswap_32(x)

#define MSDOS_MBR_SIGNATURE         0xAA55
#define EFI_PMBR_OSTYPE_EFI_GPT     0xEE
#define GPT_HEADER_SIGNATURE        0x5452415020494645ULL
#define GPT_HEADER_REVISION_V1      0x00010000
#define GPT_ENTRY_NUMBERS           128
#define GPT_ENTRY_SIZE              128
#define PART_PROPERTY_BOOTABLE      (1 << 2)

#define CALC_UNIT(x, y) (((x) + (y) - 1) / (y))

typedef union {
    struct {
        uint32_t time_low;
        uint16_t time_mid;
        uint16_t time_hi_and_version;
        uint8_t  clock_seq_hi_and_reserved;
        uint8_t  clock_seq_low;
        uint8_t  node[6];
    } uuid;
    u8 raw[16];
} efi_guid_t;

/* struct param_item is shared via recovery/parameter.h (the layout-closure
 * precheck reuses gpt_parse_params). */

/* uuid:<name>=<value> line parsed from the parameter file */
struct config_item {
    char    name[20];
    char    value[256];
};

#pragma pack(1)

struct mbr_partition {
    u8  boot_ind;
    u8  head;
    u8  sector;
    u8  cyl;
    u8  sys_ind;
    u8  end_head;
    u8  end_sector;
    u8  end_cyl;
    u32 start_sect;
    u32 nr_sects;
};

struct legacy_mbr {
    u8  boot_code[440];
    u32 unique_mbr_signature;
    u16 unknown;
    struct mbr_partition partition_record[4];
    u16 signature;
};

struct gpt_header {
    u64         signature;
    u32         revision;
    u32         header_size;
    u32         header_crc32;
    u32         reserved1;
    u64         my_lba;
    u64         alternate_lba;
    u64         first_usable_lba;
    u64         last_usable_lba;
    efi_guid_t  disk_guid;
    u64         partition_entry_lba;
    u32         num_partition_entries;
    u32         sizeof_partition_entry;
    u32         partition_entry_array_crc32;
};

union gpt_entry_attributes {
    struct {
        u64 required_to_function : 1;
        u64 no_block_io_protocol : 1;
        u64 legacy_bios_bootable : 1;
        u64 reserved : 45;
        u64 type_guid_specific : 16;
    } fields;
    u64 raw;
};

#define PARTNAME_SZ 72

struct gpt_entry {
    efi_guid_t  partition_type_guid;
    efi_guid_t  unique_partition_guid;
    u64         starting_lba;
    u64         ending_lba;
    union gpt_entry_attributes attributes;
    u16         partition_name[PARTNAME_SZ / sizeof(u16)];
};

#pragma pack()

/* ---- CRC32 (IEEE 802.3) ---- */

static const u32 crc32table_le[] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba,
    0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
    0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940,
    0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
    0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
    0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
    0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
    0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
    0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
    0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
    0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
    0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
    0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
    0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
    0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

#define DO_CRC(x) crc = tab[((crc) ^ (x)) & 255] ^ ((crc) >> 8)

static u32 gpt_crc32(u32 crc, const u8 *p, u32 len)
{
    const u32 *b = (const u32 *)p;
    const u32 *tab = crc32table_le;

    crc = crc ^ 0xFFFFFFFF;

    /* Handle unaligned head */
    if (((unsigned long)b & 3) && len) {
        do {
            const u8 *bp = (const u8 *)b;
            DO_CRC(*bp++);
            b = (const u32 *)bp;
        } while ((--len) && ((unsigned long)b & 3));
    }

    /* Process 4 bytes at a time */
    if (len >= 4) {
        u32 save_len = len & 3;
        len = len >> 2;
        --b;
        do {
            crc ^= *++b;
            DO_CRC(0);
            DO_CRC(0);
            DO_CRC(0);
            DO_CRC(0);
        } while (--len);
        b++;
        len = save_len;
    }

    /* Handle tail */
    if (len) {
        do {
            const u8 *bp = (const u8 *)b;
            DO_CRC(*bp++);
            b = (const u32 *)bp;
        } while (--len);
    }

    return crc ^ 0xFFFFFFFF;
}

/* ---- UUID helpers ---- */

static void string_to_uuid(const char *str, char *uuid)
{
    unsigned int i;
    char value;

    memset(uuid, 0, 16);
    for (i = 0; i < 256 && str[i] != '\0'; i++) {
        value = 0;
        if (str[i] >= '0' && str[i] <= '9')
            value = str[i] - '0';
        else if (str[i] >= 'a' && str[i] <= 'f')
            value = str[i] - 'a' + 10;
        else if (str[i] >= 'A' && str[i] <= 'F')
            value = str[i] - 'A' + 10;

        if ((i % 2) == 0)
            uuid[i / 2] += (value << 4);
        else
            uuid[i / 2] += value;
    }

    /* Byte-swap to match GPT mixed-endian UUID format */
    u32 *p32 = (u32 *)uuid;
    *p32 = cpu_to_be32(*p32);
    u16 *p16 = (u16 *)(uuid + 4);
    *p16 = cpu_to_be16(*p16);
    p16 = (u16 *)(uuid + 6);
    *p16 = cpu_to_be16(*p16);
}

static void gen_rand_uuid(u8 *uuid_bin)
{
    efi_guid_t id;
    u32 *ptr = (u32 *)&id;

    for (unsigned int i = 0; i < sizeof(id) / sizeof(*ptr); i++)
        *(ptr + i) = cpu_to_be32((u32)rand());

    id.uuid.time_hi_and_version =
        (id.uuid.time_hi_and_version & 0x0FFF) | 0x4000;
    id.uuid.clock_seq_hi_and_reserved =
        id.uuid.clock_seq_hi_and_reserved | 0x80;

    memcpy(uuid_bin, id.raw, sizeof(id));
}

/* ---- Fill one partition entry ---- */

static void fill_entry(struct gpt_entry *ent, struct param_item *param,
                       struct config_item *configs, int config_count,
                       u64 last_usable_lba, u64 start_lba, u64 size_lba)
{
    gen_rand_uuid(ent->partition_type_guid.raw);
    gen_rand_uuid(ent->unique_partition_guid.raw);
    ent->starting_lba = cpu_to_le64(start_lba);
    ent->ending_lba = cpu_to_le64(start_lba + size_lba - 1);
    ent->attributes.raw = 0;

    /* Check for ":bootable" / ":grow" suffixes */
    char name[20] = {0};
    strncpy(name, param->name, sizeof(name) - 1);
    char *p = strstr(name, ":");
    if (p) {
        if (strstr(name, "bootable"))
            ent->attributes.raw = PART_PROPERTY_BOOTABLE;
        if (strstr(name, "grow"))
            ent->ending_lba = cpu_to_le64(last_usable_lba);
        *p = '\0';
        strncpy(param->name, name, sizeof(param->name) - 1);
        param->name[sizeof(param->name) - 1] = '\0';
    }

    /* Set partition name (UCS-2LE) */
    for (int j = 0; j < (int)strlen(param->name); j++)
        ent->partition_name[j] = param->name[j];

    /* Match UUID from config */
    for (int j = 0; j < config_count; j++) {
        if (strcmp(configs[j].name, param->name) == 0) {
            memcpy(ent->unique_partition_guid.raw, configs[j].value, 16);
            break;
        }
    }
}

/* ============================================================
 * GPT generation — works in the device's native logical block,
 * consuming the (disk_lb, lb_bytes) pair from the backend's
 * disk_lbas op exactly as u-boot's part_efi.c consumes its
 * dev_desc (rawlba, rawblksz). The 512-byte rk sector appears
 * only where rk-format input is interpreted (parameter.txt
 * mtdparts offsets are 512-sector counts).
 * ============================================================ */

/* Build the primary table (protective MBR + header + entries) into buf. */
static void gpt_build_primary(u8 *buf, struct param_item *params,
                              int param_count, struct config_item *configs,
                              int config_count, u64 disk_lb, u32 lb_bytes)
{
    /* Logical blocks holding the entry array — u-boot: num * size / rawblksz */
    u32 entry_lb = CALC_UNIT(GPT_ENTRY_NUMBERS * GPT_ENTRY_SIZE, lb_bytes);
    u32 master_gpt_lb = 2 + entry_lb;
    /* rk-format unit boundary: parameter.txt mtdparts offsets/sizes are
     * counts of 512-byte sectors; GPT entries count logical blocks. */
    u32 lb_sectors = lb_bytes / SECTOR_SIZE;

    struct legacy_mbr *mbr = (struct legacy_mbr *)buf;
    struct gpt_header *hdr = (struct gpt_header *)(buf + lb_bytes);
    struct gpt_entry *ent = (struct gpt_entry *)(buf + 2 * lb_bytes);

    /* 1. Protective MBR (nr_sects = whole disk minus the MBR itself, as
     * u-boot's set_protective_mbr writes it: (u32)(rawlba - 1)) */
    memset(buf, 0, lb_bytes);
    mbr->signature = MSDOS_MBR_SIGNATURE;
    mbr->partition_record[0].sys_ind = EFI_PMBR_OSTYPE_EFI_GPT;
    mbr->partition_record[0].start_sect = 1;
    mbr->partition_record[0].nr_sects = cpu_to_le32((u32)(disk_lb - 1));

    /* 2. GPT header */
    memset(buf + lb_bytes, 0, lb_bytes);
    hdr->signature = cpu_to_le64(GPT_HEADER_SIGNATURE);
    hdr->revision = cpu_to_le32(GPT_HEADER_REVISION_V1);
    hdr->header_size = cpu_to_le32(sizeof(struct gpt_header));
    hdr->my_lba = cpu_to_le64(1);
    hdr->alternate_lba = cpu_to_le64(disk_lb - 1);
    hdr->first_usable_lba = cpu_to_le64(master_gpt_lb);
    hdr->last_usable_lba = cpu_to_le64(disk_lb - master_gpt_lb);
    hdr->partition_entry_lba = cpu_to_le64(2);
    hdr->num_partition_entries = cpu_to_le32(GPT_ENTRY_NUMBERS);
    hdr->sizeof_partition_entry = cpu_to_le32(GPT_ENTRY_SIZE);
    hdr->header_crc32 = 0;
    hdr->partition_entry_array_crc32 = 0;
    gen_rand_uuid(hdr->disk_guid.raw);

    /* 3. Partition entries */
    memset(buf + 2 * lb_bytes, 0, (size_t)entry_lb * lb_bytes);
    u32 backup_gpt_lb = 1 + entry_lb;
    for (int i = 0; i < param_count; i++) {
        if (params[i].name[0] == '\0')
            continue;
        u64 start = CALC_UNIT(params[i].offset, lb_sectors);
        u64 size = CALC_UNIT(params[i].size, lb_sectors);
        fill_entry(ent, &params[i], configs, config_count,
                   disk_lb - backup_gpt_lb - 1, start, size);
        ent++;
    }

    /* 4. CRC32 */
    hdr->partition_entry_array_crc32 = cpu_to_le32(
        gpt_crc32(0, buf + 2 * lb_bytes,
                   GPT_ENTRY_SIZE * GPT_ENTRY_NUMBERS));
    hdr->header_crc32 = cpu_to_le32(
        gpt_crc32(0, buf + lb_bytes, sizeof(struct gpt_header)));
}

/* Re-point the backup header (already copied from the primary) at itself. */
static void gpt_fix_backup_header(const u8 *primary, u8 *backup, u32 lb_bytes)
{
    u32 entry_lb = CALC_UNIT(GPT_ENTRY_NUMBERS * GPT_ENTRY_SIZE, lb_bytes);

    const struct gpt_header *master =
        (const struct gpt_header *)(primary + lb_bytes);
    struct gpt_header *bak =
        (struct gpt_header *)(backup + (size_t)entry_lb * lb_bytes);

    u64 val = le64_to_cpu(master->my_lba);
    bak->my_lba = master->alternate_lba;
    bak->alternate_lba = cpu_to_le64(val);
    bak->partition_entry_lba =
        cpu_to_le64(le64_to_cpu(master->alternate_lba) - entry_lb);
    bak->header_crc32 = 0;

    u32 crc = gpt_crc32(0, (const u8 *)bak,
                         le32_to_cpu(bak->header_size));
    bak->header_crc32 = cpu_to_le32(crc);
}

/*
 * Build a complete GPT image — primary table followed by backup table in one
 * contiguous calloc'd buffer (caller frees). (disk_lb, lb_bytes) is the
 * backend's disk_lbas pair (u-boot's rawlba/rawblksz). The primary
 * (*primary_bytes) is written at device offset 0; the backup (*backup_bytes,
 * starting at buf + *primary_bytes) ends exactly at the disk end:
 *     disk_lb * lb_bytes - *backup_bytes.
 * Mirrors idblock_flatten in flash_bootloader.c: the generator returns a
 * directly-writable buffer, the storage backend just writes bytes.
 */
static u8 *gpt_build(struct param_item *params, int param_count,
                     struct config_item *configs, int config_count,
                     u64 disk_lb, u32 lb_bytes,
                     size_t *primary_bytes, size_t *backup_bytes)
{
    u32 entry_lb = CALC_UNIT(GPT_ENTRY_NUMBERS * GPT_ENTRY_SIZE, lb_bytes);
    size_t pbytes = (size_t)(2 + entry_lb) * lb_bytes;
    size_t bbytes = (size_t)(1 + entry_lb) * lb_bytes;

    u8 *buf = calloc(1, pbytes + bbytes);
    if (!buf) {
        pr_err("gpt_build: oom %zu\n", pbytes + bbytes);
        return NULL;
    }
    u8 *backup = buf + pbytes;

    gpt_build_primary(buf, params, param_count, configs, config_count,
                      disk_lb, lb_bytes);

    /* Backup layout: entry array first, then header in the last block. */
    memcpy(backup, buf + 2 * lb_bytes, (size_t)entry_lb * lb_bytes);
    memcpy(backup + (size_t)entry_lb * lb_bytes, buf + lb_bytes, lb_bytes);
    gpt_fix_backup_header(buf, backup, lb_bytes);

    *primary_bytes = pbytes;
    *backup_bytes = bbytes;
    return buf;
}

/* ============================================================
 * Parameter text parsing
 * ============================================================ */

/* Exposed (recovery/parameter.h) for the upgrade layout-closure precheck. */
int gpt_parse_params(const char *param_str, struct param_item *items,
                     int max)
{
    int count = 0;
    char *buf = strdup(param_str);
    if (!buf)
        return 0;

    char *line = strtok(buf, "\n");
    while (line) {
        if (line[0] == '#') {
            line = strtok(NULL, "\n");
            continue;
        }

        char *pline = strstr(line, "mtdparts");
        if (pline && (pline = strstr(pline, ":")) != NULL) {
            pline++;
            while (*pline == ' ')
                pline++;

            char *parts = strdup(pline);
            if (!parts)
                break;

            char *token = strtok(parts, ",");
            while (token && count < max) {
                while (*token == ' ' || *token == '\t')
                    token++;

                struct param_item item = {0};
                int parsed;
                if (*token == '-') {
                    parsed = sscanf(token, "-@0x%x(%19[^)])",
                                    &item.offset, item.name);
                    if (parsed == 2) {
                        item.size = PARAM_SIZE_GROW;
                        items[count++] = item;
                    } else {
                        pr_warn("parameter: skip malformed mtdparts token '%s'\n",
                                token);
                    }
                } else {
                    parsed = sscanf(token, "0x%x@0x%x(%19[^)])",
                                    &item.size, &item.offset, item.name);
                    if (parsed == 3) {
                        items[count++] = item;
                    } else {
                        pr_warn("parameter: skip malformed mtdparts token '%s'\n",
                                token);
                    }
                }
                token = strtok(NULL, ",");
            }
            free(parts);
            break;
        }
        line = strtok(NULL, "\n");
    }

    free(buf);
    return count;
}

static int gpt_parse_uuids(const char *param_str, struct config_item *items,
                           int max)
{
    int count = 0;
    char *buf = strdup(param_str);
    if (!buf)
        return 0;

    char *line = strtok(buf, "\n");
    while (line) {
        if (line[0] == '#') {
            line = strtok(NULL, "\n");
            continue;
        }

        char *pline = strstr(line, "uuid");
        if (pline && (pline = strstr(pline, ":")) != NULL) {
            pline++;
            while (*pline == ' ')
                pline++;

            char tmp;
            char value[256] = {0};
            if (count < max) {
                sscanf(pline, "%19[^=]%c%255s", items[count].name, &tmp, value);
                string_to_uuid(value, items[count].value);
                count++;
            }
        }

        line = strtok(NULL, "\n");
    }

    free(buf);
    return count;
}

/* ============================================================
 * Engine entry
 * ============================================================ */

/* Max parameter data size: 64KB should be more than enough */
#define MAX_PARAM_SIZE  (64 * 1024)
#define MAX_PARAM_ITEMS 20
#define MAX_CONFIG_ITEMS 10

/*
 * Engine entry: flash the parameter (GPT) item. item->dev_path is a
 * whole-disk node (resolve_disk_level guarantees it in both modes) — the
 * primary GPT lands at LBA0, the backup ends exactly at the disk tail.
 */
int flash_parameter(struct upgrade_item *item, const char *image_path)
{
    pr_info("flash_parameter: %s -> %s\n", item->name, item->dev_path);

    long long param_size = item->img_size;
    if (param_size <= 0 || param_size > MAX_PARAM_SIZE) {
        pr_err("invalid parameter size: %lld\n", param_size);
        return -1;
    }

    /* Align to sector boundary */
    size_t buf_size = (size_t)param_size;
    if (buf_size % SECTOR_SIZE != 0)
        buf_size = ((buf_size / SECTOR_SIZE) + 1) * SECTOR_SIZE;

    /* 1. Read parameter data from image */
    uint8_t *data_buf = calloc(1, buf_size);
    if (!data_buf) {
        pr_err("malloc %zu bytes failed\n", buf_size);
        return -1;
    }

    int fd_src = open(image_path, O_RDONLY);
    if (fd_src < 0) {
        pr_err("open %s: %s\n", image_path, strerror(errno));
        free(data_buf);
        return -1;
    }

    if (lseek(fd_src, item->img_offset, SEEK_SET) == (off_t)-1) {
        pr_err("lseek: %s\n", strerror(errno));
        close(fd_src);
        free(data_buf);
        return -1;
    }

    if (read(fd_src, data_buf, (size_t)param_size) != (ssize_t)param_size) {
        pr_err("read parameter data: %s\n", strerror(errno));
        close(fd_src);
        free(data_buf);
        return -1;
    }
    close(fd_src);

    /* 2. Parse partition layout and UUIDs from parameter string
     * Parameter format starts at offset 8 (skip header) */
    struct param_item params[MAX_PARAM_ITEMS];
    struct config_item configs[MAX_CONFIG_ITEMS];
    memset(params, 0, sizeof(params));
    memset(configs, 0, sizeof(configs));

    int param_count = gpt_parse_params((const char *)data_buf + 8,
                                       params, MAX_PARAM_ITEMS);
    int config_count = gpt_parse_uuids((const char *)data_buf + 8,
                                       configs, MAX_CONFIG_ITEMS);

    /* No partitions parsed = malformed/empty parameter.txt. Building a GPT
     * with zero entries would wipe the partition table to an empty disk —
     * refuse rather than write it. */
    if (param_count <= 0) {
        pr_err("parameter.txt yielded no partitions — refuse to write empty GPT\n");
        free(data_buf);
        return -1;
    }

    pr_info("parsed %d partitions, %d UUID configs\n", param_count, config_count);
    for (int i = 0; i < param_count; i++) {
        pr_dbg("  param[%d] name=%s offset=0x%x size=0x%x\n",
               i, params[i].name, params[i].offset, params[i].size);
    }

    /*
     * 3. Disk geometry from the backend — the (LB count, LB size) pair,
     * u-boot's rawlba/rawblksz. Measured live: UFS (4096) and SATA (512)
     * share /dev/sd*, so only the device itself tells them apart — a global
     * storagemedia flag once forced the 4KB layout onto SATA/NOR and u-boot
     * rejected both as "Invalid GPT". The count excludes media-reserved
     * trailing blocks (NAND BBT + trailing bad blocks) — the backup GPT
     * lands exactly at its end, where u-boot looks (rawlba - 1).
     */
    if (!item->backend->disk_lbas) {
        pr_err("backend %s missing disk_lbas op\n", item->backend->name);
        free(data_buf);
        return -1;
    }
    uint32_t lb_bytes = 0;
    long long disk_lb = item->backend->disk_lbas(item->backend,
                                                 item->dev_path, &lb_bytes);
    if (disk_lb <= 0 || lb_bytes == 0) {
        pr_err("disk_lbas returned %lld (lb=%u)\n", disk_lb, lb_bytes);
        free(data_buf);
        return -1;
    }
    pr_info("GPT target %s: %lld logical blocks x %u bytes\n",
            item->dev_path, disk_lb, lb_bytes);

    /* 4. Generate primary + backup GPT in one buffer */
    size_t primary_bytes = 0, backup_bytes = 0;
    u8 *gpt_buf = gpt_build(params, param_count, configs, config_count,
                            (u64)disk_lb, lb_bytes,
                            &primary_bytes, &backup_bytes);
    if (!gpt_buf) {
        free(data_buf);
        return -1;
    }

    /* 5. Write primary GPT at the start of the device */
    int ret = item->backend->write(item->backend, item->dev_path,
                                   gpt_buf, NULL, 0, 0,
                                   (long long)primary_bytes, NULL, NULL);
    if (ret != 0) {
        pr_err("write primary GPT failed\n");
        free(gpt_buf);
        free(data_buf);
        return -1;
    }

    /* 6. Write backup GPT ending exactly at the disk end (u-boot: rawlba-1) */
    off_t backup_offset =
        (off_t)((u64)disk_lb * lb_bytes - backup_bytes);
    ret = item->backend->write(item->backend, item->dev_path,
                               gpt_buf + primary_bytes, NULL, 0,
                               backup_offset, (long long)backup_bytes,
                               NULL, NULL);
    if (ret != 0) {
        pr_err("write backup GPT failed\n");
        free(gpt_buf);
        free(data_buf);
        return -1;
    }

    pr_info("backup GPT written at offset 0x%llx\n", (long long)backup_offset);
    free(gpt_buf);
    free(data_buf);
    sync();
    pr_info("parameter flash complete\n");
    return 0;
}

#endif /* RK_GPT_UPDATE */
