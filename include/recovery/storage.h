/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_STORAGE_H
#define RK_RECOVERY_STORAGE_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Per-manager partition-path cache size. */
#define RK_PATH_CACHE_SIZE 32

/*
 * Physical storage medium. Two Linux device classes, each with its own media:
 *
 *   block class  (/dev/mmcblkX, /dev/sdX, /dev/block/by-name/X)
 *       EMMC  — eMMC; 512-byte logical block; bootable (IDBlock @ fixed offset)
 *       UFS   — SCSI; 4096-byte logical block; bootable via boot LUN /dev/sdb
 *       SATA  — SCSI; 512-byte logical block; NOT bootable (pairs with Nor)
 *   mtd class    (/dev/mtdX; medium read from /sys/class/mtd/mtdN/type)
 *       NAND  — OOB + bad-block table; nanddump/nandwrite; hosts UBI
 *       NOR   — byte-addressable, no OOB/BBT; read()/MEMERASE+write()
 *
 * UFS/SATA share /dev/sd* and NAND/NOR share /dev/mtdN, so routing that only
 * knows the *class* must probe to pick the medium — see storage.c scsi_slot /
 * mtd_slot and device_has_* in storage/device.c.
 */
enum storage_type {
    STORAGE_EMMC,
    STORAGE_UFS,
    STORAGE_SATA,
    STORAGE_NAND,
    STORAGE_NOR,
    STORAGE_TYPE_MAX,
};

/* Class predicates — express the two-tier taxonomy at call sites. */
static inline bool storage_type_is_mtd(enum storage_type t)
{
    return t == STORAGE_NAND || t == STORAGE_NOR;
}
static inline bool storage_type_is_scsi(enum storage_type t)
{
    return t == STORAGE_UFS || t == STORAGE_SATA;
}

/*
 * Vendor SSFW per-sub-image storage tag (bitmask). See dual-storage
 * patch's enum STORAGE_CODE — this is what ssfw_entry.storage carries
 * and what rk_image_hdr.storage_code[] holds for SSFW-merged items.
 *
 * Only the codes recovery actually routes are mapped to a backend in
 * storage_backend_by_code():
 *   SPINOR                     -> STORAGE_NOR
 *   SPINAND / FLASH            -> STORAGE_NAND
 *   EMMC                       -> STORAGE_EMMC
 *   SATA                       -> STORAGE_SATA
 * SD0/SD1/USB/RAM/PCIE are not full-flash targets and return NULL.
 */
#define STORAGE_FLASH_CODE      (1u << 0)
#define STORAGE_EMMC_CODE       (1u << 1)
#define STORAGE_SD0_CODE        (1u << 2)
#define STORAGE_SD1_CODE        (1u << 3)
#define STORAGE_RAM_CODE        (1u << 6)
#define STORAGE_USB_CODE        (1u << 7)
#define STORAGE_SPINAND_CODE    (1u << 8)
#define STORAGE_SPINOR_CODE     (1u << 9)
#define STORAGE_SATA_CODE       (1u << 10)
#define STORAGE_PCIE_CODE       (1u << 11)

/* Progress callback: invoked per chunk written */
typedef void (*upgrade_notify_fn)(const char *part_name,
                                  size_t written, size_t total, void *ctx);

/* Per-backend operations */
struct storage_ops {
    enum storage_type type;
    const char    *name;            /* "emmc", "mtd", "ufs" */

    /*
     * Partition-level I/O. Source/destination is EITHER an in-memory buffer
     * OR a file (exactly one of buf / path non-NULL) — so the same op serves
     * both small in-memory data (BCB/slot/GPT) and large partition images
     * without ever buffering a whole partition in RAM.
     *
     * write:
     *   buf      != NULL — write `len` bytes from memory.
     *   src_path != NULL — stream `len` bytes from src_path[src_off..] (O(1)
     *                      RAM; the large-partition path).
     * write owns media-specific preparation: NAND erases the good-block
     * runs that nandwrite -p will actually touch, NOR uses ioctl(MEMERASE)
     * RMW, and block media overwrite the existing node at the requested
     * offset. There is no separate erase op — nothing needs
     * erase-without-write (wipe_userdata reformats via storage/format.c).
     *
     * read:
     *   buf      != NULL — read `len` bytes into memory.
     *   dst_path != NULL — dump `len` bytes to dst_path (MD5 over a partition,
     *                      diff source snapshot — one nanddump on MTD).
     */
    int  (*read)(struct storage_ops *ops, const char *dev_path,
                 void *buf, const char *dst_path, off_t offset, long long len);
    int  (*write)(struct storage_ops *ops, const char *dev_path,
                  const void *buf, const char *src_path, off_t src_off,
                  off_t offset, long long len,
                  upgrade_notify_fn cb, void *cb_ctx);

    /*
     * Usable disk geometry as a (logical-block count, logical-block size)
     * pair — the counterpart of u-boot's dev_desc rawlba/rawblksz. Returns
     * the count and stores the block size in *lb_bytes; a count never
     * travels without its unit. The backup GPT must end exactly at this
     * count (u-boot reads it at rawlba−1). A function, not a constant: it
     * is measured live against `dev_path` at call time.
     *
     * Block backends: BLKGETSIZE64 / BLKSSZGET — UFS reports 4096, SATA and
     * eMMC 512; only the live device tells UFS and SATA apart (they share
     * /dev/sd*). MTD: lb is fixed 512 (u-boot's MTD rawblksz fallback); NOR
     * counts the raw device size, NAND mirrors u-boot mtd_blk_probe — skip
     * the trailing BBT reserve (NAND_BBT_SCAN_MAXBLOCKS = 4 erase blocks)
     * plus any trailing bad blocks, down to the first good one.
     *
     * Consumers: GPT geometry (uses the pair as-is, like u-boot's
     * gpt_fill_header), full-flash bounds checking and the IDBlock
     * flash_size field (both use count × lb_bytes). Returns -1 on error.
     */
    long long (*disk_lbas)(struct storage_ops *ops, const char *dev_path,
                           uint32_t *lb_bytes);

    /*
     * Device nodes, resolved once at backend create time (a registered
     * backend's medium exists, and these never change while recovery runs —
     * data, not methods).
     *
     * Disk-level writes (GPT, IDBlock, full-flash items) always address a
     * WHOLE device at absolute offsets — never a partition node, whose
     * partition-relative origin would double-offset the fixed GPT/IDBlock
     * placements.
     *
     * bare_dev: the whole-device node (/dev/mtdN master, /dev/mmcblk0,
     * /dev/sda). Block media always expose it. MTD exposes it as the
     * master device: with mtdparts in the cmdline (normal and BCB-recovery
     * boots alike — u-boot injects it unconditionally) the master is only
     * retained when the kernel sets CONFIG_MTD_PARTITIONED_MASTER=y;
     * without mtdparts (SD/USB boot) the sole mtdN is the master
     * trivially. NULL = no whole-device node visible: full-flash items
     * skip, a GPT (parameter) item aborts the upgrade.
     *
     * bootloader_dev: the whole device hosting the IDBlock at its medium's
     * fixed absolute base (idblock_geom_for). MTD master / eMMC whole
     * device — same node as bare_dev. UFS: the boot LUN /dev/sdb — NOT
     * the /dev/sda user disk, which is why this stays separate from
     * bare_dev. SATA: NULL (not bootable) — a bootloader item routed here
     * aborts the upgrade.
     */
    const char *bare_dev;
    const char *bootloader_dev;
};

/* Storage manager: multi-backend coexistence, per-partition routing. */
struct storage_mgr {
    struct storage_ops *backends[STORAGE_TYPE_MAX];
    int                 backend_count;

    /*
     * --- Private state — owned by storage/storage.c, do not touch ---
     * Flattened here (rather than behind a void *priv) because the manager
     * is process-singleton, inner_kits is empty, and PIMPL bought nothing
     * but pointer arithmetic at every access. Treat as opaque.
     */
    struct {
        char              part_name[32];
        char              dev_path[PATH_MAX];
        enum storage_type type;
    } cache[RK_PATH_CACHE_SIZE];
    int  cache_count;
};

/* Initialize storage manager: probe devices, register detected backends */
struct storage_mgr *storage_init(void);

/*
 * Route a partition name to its storage backend / resolved device node.
 * Single implementation (no vtable) — storage_mgr is a process singleton, so
 * a direct call beats a function-pointer indirection that never had a second
 * implementation.
 */
struct storage_ops *storage_resolve(struct storage_mgr *mgr,
                                    const char *part_name);
int storage_get_part_path(struct storage_mgr *mgr, const char *part_name,
                          char *path, size_t path_len);

/*
 * Look up the backend that should host a sub-image tagged with `code`
 * (a vendor STORAGE_*_CODE bitmask from ssfw_entry.storage).
 *
 * Returns the backend pointer or NULL if (a) `code` doesn't match any
 * supported medium, or (b) the matching backend wasn't registered on
 * this board. Caller must graceful-skip on NULL.
 *
 * Used by resolve_for_full_flash to route SSFW items to backends
 * without consulting by-name softlinks.
 */
struct storage_ops *storage_backend_by_code(struct storage_mgr *mgr,
                                            uint32_t code);

/* Register an additional backend at runtime */
int storage_register_backend(struct storage_mgr *mgr,
                             struct storage_ops *ops);

/*
 * Current on-device geometry of a named partition, in 512-byte sectors
 * (start sector + size). Read from sysfs via the partition's resolved node:
 * block → /sys/class/block/<node>/{start,size}; MTD → /sys/class/mtd/mtdN/
 * {offset,size} (bytes /512). The kernel reports block start/size in 512-byte
 * units regardless of logical block size, matching parameter.txt's rk-format
 * units. Returns 0 on success, -1 if the partition isn't present (no by-name /
 * no /proc/mtd entry) — the layout-closure precheck treats that as "no old
 * layout, skip". */
int storage_part_geom(struct storage_mgr *mgr, const char *name,
                      uint64_t *start_sect, uint64_t *size_sect);

#endif /* RK_RECOVERY_STORAGE_H */
