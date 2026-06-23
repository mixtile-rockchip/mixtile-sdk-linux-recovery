/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "upgrade:precheck"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#include "recovery/storage.h"
#include "recovery/mtd_info.h"
#include "recovery/log.h"
#include "upgrade_internal.h"
#if defined(RK_GPT_UPDATE)
#include "recovery/parameter.h"
#endif

/* ---- Mount check ---- */

/*
 * True if dev_path (or, when whole_disk, any partition of it) appears in
 * /proc/mounts. For an MTD node it also treats a mounted UBI volume on that
 * MTD as "mounted". whole_disk must be set only for genuine whole-device
 * nodes (bare_dev / bootloader_dev) — a partition node must not prefix-match
 * its higher-numbered siblings (mmcblk0p3 vs mmcblk0p30).
 */
static bool is_device_mounted(const char *dev_path, bool whole_disk)
{
    char real_dev[PATH_MAX];
    if (!realpath(dev_path, real_dev))
        snprintf(real_dev, sizeof(real_dev), "%s", dev_path);

    int mtd_num = mtd_parse_dev_num(real_dev);
    int ubi_num = (mtd_num >= 0) ? ubi_find_num_by_mtd(mtd_num) : -1;
    char ubi_prefix[32] = { 0 };
    if (ubi_num >= 0)
        snprintf(ubi_prefix, sizeof(ubi_prefix), "/dev/ubi%d_", ubi_num);

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return false;

    char line[512];
    bool mounted = false;
    while (fgets(line, sizeof(line), fp)) {
        char mnt_dev[256];
        if (sscanf(line, "%255s", mnt_dev) != 1)
            continue;

        /* UBI: any /dev/ubi<N>_* volume counts as "MTD N is mounted". */
        if (ubi_prefix[0] && strncmp(mnt_dev, ubi_prefix, strlen(ubi_prefix)) == 0) {
            mounted = true;
            break;
        }

        char real_mnt[PATH_MAX];
        if (!realpath(mnt_dev, real_mnt))
            continue;
        if (strcmp(real_dev, real_mnt) == 0) {
            mounted = true;
            break;
        }

        /* Whole-disk target: a mounted partition of it counts too
         * (/dev/mmcblk0p3 under /dev/mmcblk0, /dev/sda1 under /dev/sda). The
         * suffix is 'p'+digits (mmcblk/nvme) or digits (sd*). */
        if (whole_disk) {
            size_t dl = strlen(real_dev);
            if (strncmp(real_mnt, real_dev, dl) == 0) {
                char c = real_mnt[dl];
                if (c == 'p' || (c >= '0' && c <= '9')) {
                    mounted = true;
                    break;
                }
            }
        }
    }
    fclose(fp);
    return mounted;
}

#if defined(RK_GPT_UPDATE)
/* ---- GPT mounted-layout and layout-closure guards ---- */

struct gpt_precheck {
    struct upgrade_item *item;
    struct param_item    parts[40];
    int                  count;
};

/* Compare partition names after stripping Rockchip GPT properties and _a/_b. */
static size_t closure_base_len(const char *name)
{
    size_t len = strcspn(name, ":");
    if (len >= 2 && name[len - 2] == '_' &&
        (name[len - 1] == 'a' || name[len - 1] == 'b'))
        len -= 2;
    return len;
}

static bool name_base_eq(const char *a, const char *b)
{
    size_t la = closure_base_len(a);
    size_t lb = closure_base_len(b);
    return la == lb && strncmp(a, b, la) == 0;
}

static void partition_lookup_name(const char *name, char *out, size_t out_sz)
{
    size_t len = strcspn(name, ":");
    if (len >= out_sz)
        len = out_sz - 1;
    memcpy(out, name, len);
    out[len] = '\0';
}

/* True if a partition with this base name is among the items being flashed. */
static bool name_in_items(struct upgrade_item *items, int count,
                          const char *name)
{
    for (int i = 0; i < count; i++)
        if (items[i].need_update && name_base_eq(items[i].name, name))
            return true;
    return false;
}

static struct upgrade_item *find_gpt_item(struct upgrade_item *items, int count)
{
    for (int i = 0; i < count; i++)
        if (items[i].need_update && items[i].type == ITEM_GPT)
            return &items[i];
    return NULL;
}

/* Read the parameter.txt text region (after the 8-byte PARM header, matching
 * flash_parameter.c) from the staged image into buf. Returns parse-start
 * pointer, or NULL on error. */
static const char *read_param_text(const char *image_path, long long off,
                                   long long size, char *buf, size_t bufsz)
{
    if (off < 0 || size <= 8)
        return NULL;
    size_t want = ((long long)bufsz - 1 < size) ? bufsz - 1 : (size_t)size;
    int fd = open(image_path, O_RDONLY);
    if (fd < 0)
        return NULL;
    if (lseek(fd, (off_t)off, SEEK_SET) == (off_t)-1) {
        close(fd);
        return NULL;
    }
    ssize_t n = read(fd, buf, want);
    close(fd);
    if (n <= 8)
        return NULL;
    buf[n] = '\0';
    return buf + 8;     /* skip the 8-byte PARM header */
}

static int load_gpt_precheck(struct upgrade_context *ctx,
                             struct upgrade_item *items, int count,
                             struct gpt_precheck *gpt)
{
    memset(gpt, 0, sizeof(*gpt));
    gpt->item = find_gpt_item(items, count);
    if (!gpt->item)
        return 0;   /* not rewriting the GPT */

    char text[8192];
    const char *p = read_param_text(ctx->image_path, gpt->item->img_offset,
                                    gpt->item->img_size, text, sizeof(text));
    if (!p) {
        pr_err("GPT rewrite requested but cannot read parameter.txt "
               "from image — abort\n");
        return -1;
    }

    gpt->count = gpt_parse_params(p, gpt->parts,
                                  (int)(sizeof(gpt->parts) /
                                        sizeof(gpt->parts[0])));
    if (gpt->count <= 0) {
        pr_err("GPT rewrite requested but new parameter has no mtdparts "
               "— abort\n");
        return -1;
    }
    return 0;
}

static const char *path_base(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool block_node_exists(const char *real_dev)
{
    char sys[128];
    snprintf(sys, sizeof(sys), "/sys/class/block/%.64s", path_base(real_dev));
    return access(sys, F_OK) == 0;
}

static bool block_child_of_disk(const char *real_disk, const char *real_part)
{
    size_t dl = strlen(real_disk);

    if (strncmp(real_part, real_disk, dl) != 0)
        return false;

    char c = real_part[dl];
    return c == 'p' || (c >= '0' && c <= '9');
}

static int block_part_geom_from_real(const char *real_dev,
                                     uint64_t *start_sect,
                                     uint64_t *size_sect)
{
    char sys[128];
    unsigned long long v;
    FILE *fp;

    snprintf(sys, sizeof(sys), "/sys/class/block/%.64s/start", path_base(real_dev));
    fp = fopen(sys, "r");
    if (!fp)
        return -1;
    if (fscanf(fp, "%llu", &v) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *start_sect = (uint64_t)v;

    snprintf(sys, sizeof(sys), "/sys/class/block/%.64s/size", path_base(real_dev));
    fp = fopen(sys, "r");
    if (!fp)
        return -1;
    if (fscanf(fp, "%llu", &v) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *size_sect = (uint64_t)v;
    return 0;
}

static int find_new_part_by_old_geom(struct upgrade_context *ctx,
                                     const struct gpt_precheck *gpt,
                                     uint64_t old_start, uint64_t old_size)
{
    for (int i = 0; i < gpt->count; i++) {
        char lookup[sizeof(gpt->parts[i].name)];
        uint64_t cur_start = 0, cur_size = 0;

        partition_lookup_name(gpt->parts[i].name, lookup, sizeof(lookup));
        if (storage_part_geom(ctx->storage, lookup,
                              &cur_start, &cur_size) != 0)
            continue;
        if (cur_start == old_start && cur_size == old_size)
            return i;
    }
    return -1;
}

static int check_mounted_gpt_child(struct upgrade_context *ctx,
                                   const struct gpt_precheck *gpt,
                                   const char *mnt_dev,
                                   const char *real_mnt)
{
    uint64_t old_start = 0, old_size = 0;

    if (block_part_geom_from_real(real_mnt, &old_start, &old_size) != 0) {
        pr_err("GPT precheck: mounted partition %s has unknown geometry — "
               "abort\n", mnt_dev);
        return -1;
    }

    int idx = find_new_part_by_old_geom(ctx, gpt, old_start, old_size);
    if (idx < 0) {
        pr_err("GPT precheck: mounted partition %s (start=0x%llx size=0x%llx) "
               "is not represented in the new parameter layout — abort\n",
               mnt_dev, (unsigned long long)old_start,
               (unsigned long long)old_size);
        return -1;
    }

    const struct param_item *np = &gpt->parts[idx];
    if (np->offset != old_start) {
        pr_err("GPT precheck: mounted partition %s/%s start moves "
               "0x%llx->0x%x — abort\n", mnt_dev, np->name,
               (unsigned long long)old_start, np->offset);
        return -1;
    }

    if (np->size != PARAM_SIZE_GROW && np->size < old_size) {
        pr_err("GPT precheck: mounted partition %s/%s shrinks "
               "0x%llx->0x%x sectors — abort\n", mnt_dev, np->name,
               (unsigned long long)old_size, np->size);
        return -1;
    }

    pr_info("GPT precheck: mounted partition %s/%s keeps start and does not "
            "shrink — allow GPT write (reboot required)\n", mnt_dev, np->name);
    return 0;
}

/*
 * ITEM_GPT is allowed on a disk that has mounted child partitions only when
 * every mounted child is provably safe under the new GPT: same start and not
 * smaller. This supports the common SDK OTA case where update.img is staged on
 * /userdata while parameter only preserves or grows that partition.
 */
static int gpt_mounted_layout_check(struct upgrade_context *ctx,
                                    const struct gpt_precheck *gpt)
{
    char real_disk[PATH_MAX];
    if (!realpath(gpt->item->dev_path, real_disk))
        snprintf(real_disk, sizeof(real_disk), "%s", gpt->item->dev_path);

    /* Non-block GPT targets (for example MTD parameter) keep the conservative
     * guard; there is no kernel child-partition table to compare safely. */
    if (!block_node_exists(real_disk))
        return is_device_mounted(gpt->item->dev_path, gpt->item->whole_device)
                   ? -1 : 0;

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return 0;

    char line[512];
    int ret = 0;
    while (fgets(line, sizeof(line), fp)) {
        char mnt_dev[256];
        char real_mnt[PATH_MAX];

        if (sscanf(line, "%255s", mnt_dev) != 1)
            continue;
        if (!realpath(mnt_dev, real_mnt))
            continue;

        if (strcmp(real_disk, real_mnt) == 0) {
            pr_err("GPT precheck: whole disk %s is mounted — abort\n",
                   gpt->item->dev_path);
            ret = -1;
            break;
        }
        if (!block_child_of_disk(real_disk, real_mnt))
            continue;

        if (check_mounted_gpt_child(ctx, gpt, mnt_dev, real_mnt) != 0) {
            ret = -1;
            break;
        }
    }
    fclose(fp);
    return ret;
}

static bool ranges_overlap(uint64_t a_start, uint64_t a_size,
                           uint64_t b_start, uint64_t b_size)
{
    uint64_t a_end = a_start + a_size;
    uint64_t b_end = b_start + b_size;

    return a_start < b_end && b_start < a_end;
}

/* When local OTA uses GPT-in-set, normal partitions are addressed through the
 * whole disk so their offsets match the new GPT. A mounted sibling partition
 * on that disk is safe as long as this item does not overlap it. */
static int whole_disk_region_mounted(const struct upgrade_item *it)
{
    char real_disk[PATH_MAX];
    if (!realpath(it->dev_path, real_disk))
        snprintf(real_disk, sizeof(real_disk), "%s", it->dev_path);

    if (!block_node_exists(real_disk))
        return is_device_mounted(it->dev_path, it->whole_device) ? -1 : 0;

    uint64_t write_start = (uint64_t)it->flash_offset;
    uint64_t write_size = (uint64_t)it->img_size;

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return 0;

    char line[512];
    int ret = 0;
    while (fgets(line, sizeof(line), fp)) {
        char mnt_dev[256];
        char real_mnt[PATH_MAX];
        uint64_t mnt_start = 0, mnt_size = 0;

        if (sscanf(line, "%255s", mnt_dev) != 1)
            continue;
        if (!realpath(mnt_dev, real_mnt))
            continue;

        if (strcmp(real_disk, real_mnt) == 0) {
            pr_err("precheck: %s target disk %s is mounted — abort\n",
                   it->name, it->dev_path);
            ret = -1;
            break;
        }
        if (!block_child_of_disk(real_disk, real_mnt))
            continue;
        if (block_part_geom_from_real(real_mnt, &mnt_start, &mnt_size) != 0) {
            pr_err("precheck: mounted partition %s has unknown geometry — "
                   "abort\n", mnt_dev);
            ret = -1;
            break;
        }

        uint64_t mnt_start_b = mnt_start * (uint64_t)SECTOR_SIZE;
        uint64_t mnt_size_b = mnt_size * (uint64_t)SECTOR_SIZE;
        if (ranges_overlap(write_start, write_size, mnt_start_b, mnt_size_b)) {
            pr_err("precheck: %s write range 0x%llx+0x%llx overlaps mounted "
                   "%s (0x%llx+0x%llx) — abort\n", it->name,
                   (unsigned long long)write_start,
                   (unsigned long long)write_size, mnt_dev,
                   (unsigned long long)mnt_start_b,
                   (unsigned long long)mnt_size_b);
            ret = -1;
            break;
        }
    }
    fclose(fp);
    return ret;
}

/*
 * GPT layout-closure guard. When the upgrade rewrites the GPT, a partition
 * whose start moves in the new table but isn't being reflashed would have its
 * data left at the old offset -> misplaced. Compare the new parameter.txt
 * layout against the board's current layout:
 *   - start changed, not in upgrade list             -> FATAL (abort)
 *   - size changed (same start), non-grow, not flashed -> WARN
 *   - grow partition (parameter '-@', size sentinel) -> exempt
 *   - new partition (no old geometry), not flashed   -> WARN
 * No old layout (virgin board / SD-USB) -> nothing to compare, pass.
 */
static int gpt_layout_closure(struct upgrade_context *ctx,
                              const struct gpt_precheck *gpt,
                              struct upgrade_item *items, int count)
{
    if (!gpt->item)
        return 0;   /* not rewriting the GPT */

    int fatal = 0;
    for (int i = 0; i < gpt->count; i++) {
        char lookup[sizeof(gpt->parts[i].name)];
        partition_lookup_name(gpt->parts[i].name, lookup, sizeof(lookup));

        uint64_t old_start = 0, old_size = 0;
        bool in_list = name_in_items(items, count, lookup);

        if (storage_part_geom(ctx->storage, lookup,
                              &old_start, &old_size) != 0) {
            if (!in_list)
                pr_warn("closure: new partition '%s' not in upgrade list "
                        "(left unwritten)\n", gpt->parts[i].name);
            continue;
        }

        if (old_start != gpt->parts[i].offset) {
            if (in_list) {
                pr_dbg("closure: '%s' moved 0x%llx->0x%x, reflashed — ok\n",
                       gpt->parts[i].name, (unsigned long long)old_start,
                       gpt->parts[i].offset);
            } else {
                pr_err("closure: '%s' start moves 0x%llx->0x%x but it is NOT "
                       "in the upgrade list — its data would be misplaced; "
                       "abort (include it in --upgrade=)\n",
                       gpt->parts[i].name, (unsigned long long)old_start,
                       gpt->parts[i].offset);
                fatal = 1;
            }
        } else if (gpt->parts[i].size != PARAM_SIZE_GROW &&
                   old_size != gpt->parts[i].size && !in_list) {
            pr_warn("closure: '%s' resized (0x%llx->0x%x sect, start unchanged) "
                    "but not reflashed — tolerated\n",
                    gpt->parts[i].name, (unsigned long long)old_size,
                    gpt->parts[i].size);
        }
    }

    if (fatal) {
        if (ctx->print_cb)
            ctx->print_cb((char *)"GPT layout change leaves a moved partition "
                                  "unflashed — abort\n", ctx->cb_ctx);
        return -1;
    }
    return 0;
}
#endif /* RK_GPT_UPDATE */

/*
 * Pre-flight gate: validate every planned item before writing a single byte,
 * so a predictable failure (target mounted, image larger than its target)
 * aborts the upgrade instead of bricking the board half-written. Source
 * integrity is already covered by image_parse's whole-package MD5; this
 * covers the device side. Returns 0 if all clear, -1 to abort.
 */
int precheck_items(struct upgrade_context *ctx,
                   struct upgrade_item *items, int count)
{
    char prompt[128];
#if defined(RK_GPT_UPDATE)
    struct gpt_precheck gpt;
    if (load_gpt_precheck(ctx, items, count, &gpt) != 0)
        return -1;
#endif

    for (int i = 0; i < count; i++) {
        struct upgrade_item *it = &items[i];
        if (!it->need_update || !it->flash || !it->backend)
            continue;

        /* Mount guard. GPT gets a layout-aware check so a mounted
         * source partition (typically /userdata/update.img) may remain mounted
         * when its start is preserved and it does not shrink. In local OTA with
         * GPT-in-set, normal partitions are whole-disk addressed; reject only
         * if this item's write range overlaps a mounted child partition.
         * Everything else keeps the conservative whole-device guard. */
#if defined(RK_GPT_UPDATE)
        if (it->type == ITEM_GPT && it == gpt.item) {
            if (gpt_mounted_layout_check(ctx, &gpt) != 0) {
                if (ctx->print_cb) {
                    snprintf(prompt, sizeof(prompt),
                             "[%s] mounted layout is unsafe\n", it->name);
                    ctx->print_cb(prompt, ctx->cb_ctx);
                }
                return -1;
            }
        } else if (gpt.item && ctx->source_type == SRC_LOCAL &&
                   it->type == ITEM_PARTITION && it->whole_device) {
            if (whole_disk_region_mounted(it) != 0) {
                if (ctx->print_cb) {
                    snprintf(prompt, sizeof(prompt),
                             "[%s] target overlaps mounted partition\n",
                             it->name);
                    ctx->print_cb(prompt, ctx->cb_ctx);
                }
                return -1;
            }
        } else
#endif
        if (is_device_mounted(it->dev_path, it->whole_device)) {
            pr_err("precheck: %s target %s is mounted (or a partition of it is)"
                   " — abort before writing\n", it->name, it->dev_path);
            if (ctx->print_cb) {
                snprintf(prompt, sizeof(prompt),
                         "[%s] target is mounted, cannot upgrade\n", it->name);
                ctx->print_cb(prompt, ctx->cb_ctx);
            }
            return -1;
        }

        /* Capacity fit: flash_offset + img_size must fit the target node.
         * disk_lbas gives (count, lb): a block by-name partition node reports
         * its own size, a whole disk the disk size, MTD MEMGETINFO (NAND minus
         * the BBT reserve, i.e. conservative). GPT/IDBlock self-place with
         * small img_size at offset 0 so this is a floor for them and the real
         * guard for partition images, including OTA by-name (never checked
         * before). */
        if (it->img_size > 0 && it->backend->disk_lbas) {
            uint32_t lb = 0;
            long long lbas = it->backend->disk_lbas(it->backend,
                                                    it->dev_path, &lb);
            long long cap = (lbas > 0 && lb > 0)
                                ? lbas * (long long)lb : -1;
            /* Geometry probe failed. For a whole-device target (GPT/IDBlock/
             * full-flash) the capacity is geometry-critical — writing blind
             * could overrun the device, so abort rather than guess. A plain
             * OTA by-name partition tolerates it (BLKGETSIZE64 failing on a
             * resolved partition node is near-impossible and non-fatal). */
            if (cap <= 0 && it->whole_device) {
                pr_err("precheck: %s disk_lbas failed for whole-device target "
                       "%s — capacity unknown, abort\n", it->name, it->dev_path);
                if (ctx->print_cb) {
                    snprintf(prompt, sizeof(prompt),
                             "[%s] capacity probe failed\n", it->name);
                    ctx->print_cb(prompt, ctx->cb_ctx);
                }
                return -1;
            }
            if (cap > 0 && it->flash_offset + it->img_size > cap) {
                pr_err("precheck: %s @0x%llx+0x%llx exceeds %s capacity 0x%llx"
                       " — oversized image, or MTD split by mtdparts"
                       " (full-flash needs the whole device) — abort\n",
                       it->name, it->flash_offset, it->img_size,
                       it->dev_path, cap);
                if (ctx->print_cb) {
                    snprintf(prompt, sizeof(prompt),
                             "[%s] image exceeds target size\n", it->name);
                    ctx->print_cb(prompt, ctx->cb_ctx);
                }
                return -1;
            }
        }
    }

#if defined(RK_GPT_UPDATE)
    /* GPT rewrite: a moved partition that isn't being reflashed = misplaced
     * data. Check old (on-device) vs new (parameter.txt) layout. */
    if (gpt_layout_closure(ctx, &gpt, items, count) != 0)
        return -1;
#endif

    return 0;
}
