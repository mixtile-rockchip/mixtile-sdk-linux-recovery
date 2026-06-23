/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_UBI_H
#define RK_RECOVERY_UBI_H

struct ubi_ops {
    /* Attach MTD partition to UBI. Returns ubi_num on success, -1 on error. */
    int (*attach)(int mtd_num);

    /* Detach UBI. Returns 0 on success. */
    int (*detach)(int ubi_num);

    /* Read UBI volume contents to a file. out_size receives data_bytes. */
    int (*read_volume)(int ubi_num, int vol_id,
                       const char *out_file, long long *out_size);

    /* Write a file into an existing UBI volume (e.g. /dev/ubi0_0). */
    int (*update_volume)(const char *ubi_dev, const char *src_file);

    /* Format MTD → ubiformat → ubiattach → ubimkvol with vol_name. */
    int (*format)(int mtd_num, const char *vol_name);
};

/*
 * Returns the singleton UBI ops, or NULL if UBI support is not compiled in
 * (i.e. RK_STORAGE_MTD=0).
 */
struct ubi_ops *ubi_get_ops(void);

#endif /* RK_RECOVERY_UBI_H */
