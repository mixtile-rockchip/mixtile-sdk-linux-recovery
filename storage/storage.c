/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "storage"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "recovery/storage.h"
#include "recovery/device.h"
#include "recovery/config.h"
#include "recovery/log.h"
#include "recovery/mtd_info.h"

/* Forward declarations for backend registration */
#if defined(RK_STORAGE_EMMC)
extern struct storage_ops *emmc_backend_create(void);
#endif
#if defined(RK_STORAGE_MTD)
extern struct storage_ops *nand_backend_create(void);
extern struct storage_ops *nor_backend_create(void);
#endif
#if defined(RK_STORAGE_UFS)
extern struct storage_ops *ufs_backend_create(void);
extern struct storage_ops *sata_backend_create(void);
#endif

/*
 * Partition path cache and bootloader path are now flattened into
 * struct storage_mgr (see include/recovery/storage.h). The previous
 * PIMPL-with-pointer-arithmetic split (one calloc + (char*)mgr+sizeof(...))
 * was fragile (alignment) and bought no real encapsulation because
 * inner_kits is empty and the manager is a process singleton.
 */

/* ---- medium-slot helpers: classes whose media share a device-node space ----
 *
 * by-name links / /proc/mtd only reveal the *class* (a /dev/sd* or /dev/mtdN
 * node). These pick the concrete medium so routing lands on the right backend.
 */

/* UFS and SATA both expose /dev/sd*; a board registers exactly one. */
static enum storage_type scsi_slot(struct storage_mgr *mgr)
{
    if (mgr->backends[STORAGE_UFS])
        return STORAGE_UFS;
    if (mgr->backends[STORAGE_SATA])
        return STORAGE_SATA;
    return STORAGE_TYPE_MAX;
}

/* NAND and NOR both live behind /dev/mtdN; classify by /sys/class/mtd type.
 * Returns the medium's slot if registered, else STORAGE_TYPE_MAX. */
static enum storage_type mtd_slot_for_num(struct storage_mgr *mgr, int num)
{
    if (num < 0)
        return STORAGE_TYPE_MAX;
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/mtd%d", num);
    enum storage_type t = mtd_is_nor(dev) ? STORAGE_NOR : STORAGE_NAND;
    return mgr->backends[t] ? t : STORAGE_TYPE_MAX;
}

/* ---- resolve: route partition name to storage backend ---- */

struct storage_ops *storage_resolve(struct storage_mgr *mgr,
                                    const char *part_name)
{
    /* Check cache first */
    for (int i = 0; i < mgr->cache_count; i++) {
        if (strcmp(mgr->cache[i].part_name, part_name) == 0) {
            enum storage_type t = mgr->cache[i].type;
            return mgr->backends[t];
        }
    }

    /* Step 1: by-name softlink, with explicit three-branch link-target match.
     *
     * On RK platforms the by-name symlink is the authoritative routing hint:
     *   mtdblock<N>  → NAND/NOR  (by mtd type; char device /dev/mtdN for writes)
     *   mmcblk<N>    → EMMC
     *   sd<X> / nvme → UFS or SATA (the registered SCSI medium) */
    char by_name_path[256];
    snprintf(by_name_path, sizeof(by_name_path), "%s/%s",
             RK_BLOCK_BY_NAME, part_name);

    struct stat sb;
    if (lstat(by_name_path, &sb) == 0) {
        char link_target[256] = {0};
        ssize_t n = readlink(by_name_path, link_target, sizeof(link_target) - 1);
        if (n > 0)
            link_target[n] = '\0';

        enum storage_type t = STORAGE_TYPE_MAX;
        if (strstr(link_target, "mtdblock") || strstr(link_target, "/mtd"))
            t = mtd_slot_for_num(mgr, mtd_parse_dev_num(link_target));
        else if (strstr(link_target, "mmcblk"))
            t = STORAGE_EMMC;
        else if (strstr(link_target, "/sd") || strstr(link_target, "nvme"))
            t = scsi_slot(mgr);

        if (t != STORAGE_TYPE_MAX && mgr->backends[t]) {
            /* Cache the result. For MTD we leave dev_path empty so
             * get_part_path re-resolves to /dev/mtdN (char device, not
             * mtdblock). For block backends, cache the by-name path. */
            if (mgr->cache_count < RK_PATH_CACHE_SIZE) {
                int idx = mgr->cache_count++;
                snprintf(mgr->cache[idx].part_name,
                         sizeof(mgr->cache[idx].part_name), "%s", part_name);
                if (storage_type_is_mtd(t))
                    mgr->cache[idx].dev_path[0] = '\0';
                else
                    snprintf(mgr->cache[idx].dev_path,
                             sizeof(mgr->cache[idx].dev_path), "%s", by_name_path);
                mgr->cache[idx].type = t;
            }
            pr_dbg("resolve %s → backend %d via by-name (%s)\n",
                   part_name, t, link_target);
            return mgr->backends[t];
        }
        if (t == STORAGE_TYPE_MAX)
            pr_warn("by-name '%s' → '%s': unrecognized link target\n",
                    part_name, link_target);
        else
            pr_warn("by-name '%s' → backend %d not registered, fall through\n",
                    part_name, t);
    }

#if defined(RK_STORAGE_MTD)
    /* Step 2: /proc/mtd scan fallback.
     * Some BSPs don't symlink MTD partitions into /dev/block/by-name/
     * (rk3588 Nor+SATA实测：uboot 没 by-name 软链)。靠 partition name 在
     * /proc/mtd 查到 mtd 编号，再按 type 分到 NAND / NOR backend。 */
    {
        enum storage_type mt =
            mtd_slot_for_num(mgr, mtd_find_num_by_name(part_name));
        if (mt != STORAGE_TYPE_MAX) {
            if (mgr->cache_count < RK_PATH_CACHE_SIZE) {
                int idx = mgr->cache_count++;
                snprintf(mgr->cache[idx].part_name,
                         sizeof(mgr->cache[idx].part_name), "%s", part_name);
                mgr->cache[idx].dev_path[0] = '\0';
                mgr->cache[idx].type = mt;
            }
            pr_dbg("resolve %s → mtd backend %d via /proc/mtd scan\n",
                   part_name, mt);
            return mgr->backends[mt];
        }
    }
#endif

    pr_dbg("resolve %s → not present on this device\n", part_name);
    return NULL;
}

/* ---- get_part_path: get device path for partition name ---- */

int storage_get_part_path(struct storage_mgr *mgr, const char *part_name,
                          char *path, size_t path_len)
{
    /* Ensure routing is resolved & cache populated. resolve() is idempotent
     * for already-cached entries, so this is safe whether the caller invoked
     * resolve() first or not. Without this, a first-time call here would take
     * the by-name shortcut and hand an MTD partition back as /dev/block/by-name/
     * (mtdblock), which MTD backends reject as non-char. */
    (void)storage_resolve(mgr, part_name);

    /* Check cache */
    int cached_mtd = 0;
    for (int i = 0; i < mgr->cache_count; i++) {
        if (strcmp(mgr->cache[i].part_name, part_name) == 0) {
            if (mgr->cache[i].dev_path[0]) {
                snprintf(path, path_len, "%s", mgr->cache[i].dev_path);
                return 0;
            }
            if (storage_type_is_mtd(mgr->cache[i].type))
                cached_mtd = 1;
            break;
        }
    }

    /* Try block device by-name — skip if routing is known to be MTD, since
     * MTD backend needs /dev/mtdN (char), not /dev/block/by-name/ (mtdblock). */
    if (!cached_mtd) {
        char by_name[256];
        snprintf(by_name, sizeof(by_name), "%s/%s", RK_BLOCK_BY_NAME, part_name);
        if (access(by_name, F_OK) == 0) {
            snprintf(path, path_len, "%s", by_name);
            return 0;
        }
    }

    /* Try MTD: look up partition number and format as /dev/mtdN */
    int mtd_num = mtd_find_num_by_name(part_name);
    if (mtd_num >= 0) {
        snprintf(path, path_len, "/dev/mtd%d", mtd_num);
        return 0;
    }

    return -1;
}

/* ---- Public API ---- */

struct storage_ops *storage_backend_by_code(struct storage_mgr *mgr,
                                            uint32_t code)
{
    /* Vendor STORAGE_CODE is a bitmask; in practice each ssfw_entry sets
     * exactly one bit, but we tolerate combined masks (e.g. a vendor
     * tagging "any NAND" with FLASH|SPINAND). Resolution order: pick
     * the first matching bit. */
    enum storage_type want;

    if (code & STORAGE_SPINOR_CODE)
        want = STORAGE_NOR;
    else if (code & (STORAGE_SPINAND_CODE | STORAGE_FLASH_CODE))
        want = STORAGE_NAND;    /* FLASH = legacy raw NAND */
    else if (code & STORAGE_EMMC_CODE)
        want = STORAGE_EMMC;
    else if (code & STORAGE_SATA_CODE)
        want = STORAGE_SATA;
    else {
        pr_warn("backend_by_code: 0x%x not routable (SD/USB/RAM/PCIE unsupported)\n",
                code);
        return NULL;
    }

    struct storage_ops *ops = mgr->backends[want];
    if (!ops)
        pr_warn("backend_by_code: 0x%x → type %d not registered on this board\n",
                code, want);
    return ops;
}

int storage_register_backend(struct storage_mgr *mgr, struct storage_ops *ops)
{
    if (ops->type >= STORAGE_TYPE_MAX)
        return -1;
    if (mgr->backends[ops->type]) {
        pr_warn("backend %s already registered\n", ops->name);
        return -1;
    }
    mgr->backends[ops->type] = ops;
    mgr->backend_count++;
    pr_info("registered storage backend: %s\n", ops->name);
    return 0;
}

struct storage_mgr *storage_init(void)
{
    struct storage_mgr *mgr = calloc(1, sizeof(*mgr));
    if (!mgr)
        return NULL;

    /* Initialize MMC point cache once (used by emmc_backend internals). */
    device_init_mmc_points();

    /*
     * Auto-detect and register backends — "register if present" model.
     * Probes runtime evidence (/proc/mtd, /sys/bus/mmc, /sys/class/scsi_host)
     * rather than cmdline `storagemedia=` which is mutually exclusive and
     * can't represent dual-storage setups (e.g. rk3588 Nor + SATA reports
     * storagemedia=scsi but Nor is still attached via sfc_nor when dts
     * enables it).
     */
#if defined(RK_STORAGE_EMMC)
    if (device_has_emmc()) {
        struct storage_ops *emmc = emmc_backend_create();
        if (emmc)
            storage_register_backend(mgr, emmc);
    }
#endif

#if defined(RK_STORAGE_MTD)
    /* NAND and NOR are distinct media sharing /dev/mtdN; register each that
     * is physically present (a board may even carry both). */
    if (device_has_nand()) {
        struct storage_ops *nand = nand_backend_create();
        if (nand)
            storage_register_backend(mgr, nand);
    }
    if (device_has_nor()) {
        struct storage_ops *nor = nor_backend_create();
        if (nor)
            storage_register_backend(mgr, nor);
    }
#endif

#if defined(RK_STORAGE_UFS)
    /* UFS and SATA are distinct SCSI media sharing /dev/sd*; a board has at
     * most one (probe_scsi_subtype picks). Register whichever is present. */
    if (device_has_ufs()) {
        struct storage_ops *ufs = ufs_backend_create();
        if (ufs)
            storage_register_backend(mgr, ufs);
    }
    if (device_has_sata()) {
        struct storage_ops *sata = sata_backend_create();
        if (sata)
            storage_register_backend(mgr, sata);
    }
#endif

    if (mgr->backend_count == 0)
        pr_warn("no storage backends detected\n");
    else
        pr_info("storage init: %d backend(s)\n", mgr->backend_count);

    return mgr;
}

/* Read a single unsigned decimal value from a sysfs attribute file. */
static int read_u64_file(const char *path, uint64_t *out)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    unsigned long long v = 0;
    int n = fscanf(fp, "%llu", &v);
    fclose(fp);
    if (n != 1)
        return -1;
    *out = (uint64_t)v;
    return 0;
}

int storage_part_geom(struct storage_mgr *mgr, const char *name,
                      uint64_t *start_sect, uint64_t *size_sect)
{
    if (!mgr || !name || !start_sect || !size_sect)
        return -1;

    char node[PATH_MAX];
    if (storage_get_part_path(mgr, name, node, sizeof(node)) != 0)
        return -1;      /* partition not present on this board */

    int mtdn = mtd_parse_dev_num(node);
    if (mtdn >= 0) {
        /* MTD sysfs reports offset/size in bytes. */
        char p[64];
        uint64_t off_b = 0, sz_b = 0;
        snprintf(p, sizeof(p), "/sys/class/mtd/mtd%d/offset", mtdn);
        if (read_u64_file(p, &off_b) != 0)
            return -1;
        snprintf(p, sizeof(p), "/sys/class/mtd/mtd%d/size", mtdn);
        if (read_u64_file(p, &sz_b) != 0)
            return -1;
        *start_sect = off_b / SECTOR_SIZE;
        *size_sect  = sz_b / SECTOR_SIZE;
        return 0;
    }

    /* Block: resolve to the kernel node name; /sys/class/block/<node>/start
     * and size are already in 512-byte sectors. */
    char real[PATH_MAX];
    if (!realpath(node, real))
        snprintf(real, sizeof(real), "%s", node);
    const char *slash = strrchr(real, '/');
    const char *base = slash ? slash + 1 : real;

    /* base is a kernel device node name (mmcblk0p5, sda1, ...) — short; the
     * %.64s bound keeps the path within sys[] and silences -Wformat-truncation. */
    char sys[128];
    snprintf(sys, sizeof(sys), "/sys/class/block/%.64s/start", base);
    if (read_u64_file(sys, start_sect) != 0)
        return -1;
    snprintf(sys, sizeof(sys), "/sys/class/block/%.64s/size", base);
    if (read_u64_file(sys, size_sect) != 0)
        return -1;
    return 0;
}
