/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "mtd_info"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "recovery/mtd_info.h"
#include "recovery/log.h"

#define PROC_MTD        "/proc/mtd"
#define UBI_SYS_FMT     "/sys/class/ubi/ubi%d/mtd_num"
#define UBI_SEARCH_MAX  16

int mtd_find_num_by_name(const char *part_name)
{
    if (!part_name || !*part_name)
        return -1;

    FILE *fp = fopen(PROC_MTD, "r");
    if (!fp)
        return -1;

    int mtd_num = -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int num;
        char name[64];
        if (sscanf(line, "mtd%d: %*s %*s \"%63[^\"]\"", &num, name) == 2) {
            if (strcmp(name, part_name) == 0) {
                mtd_num = num;
                break;
            }
        }
    }
    fclose(fp);
    return mtd_num;
}

bool mtd_name_exists(const char *part_name)
{
    return mtd_find_num_by_name(part_name) >= 0;
}

bool mtd_any_exists(void)
{
    FILE *fp = fopen(PROC_MTD, "r");
    if (!fp)
        return false;

    bool found = false;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int num;
        if (sscanf(line, "mtd%d:", &num) == 1) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

int mtd_parse_dev_num(const char *mtd_path)
{
    if (!mtd_path)
        return -1;

    /* Accept /dev/mtdN, /dev/mtdblockN, or bare basenames mtdN / mtdblockN. */
    const char *base = strrchr(mtd_path, '/');
    base = base ? base + 1 : mtd_path;

    const char *num_str = NULL;
    if (strncmp(base, "mtdblock", 8) == 0)
        num_str = base + 8;
    else if (strncmp(base, "mtd", 3) == 0)
        num_str = base + 3;
    else
        return -1;

    if (*num_str < '0' || *num_str > '9')
        return -1;

    char *end = NULL;
    long n = strtol(num_str, &end, 10);
    if (end == num_str || *end != '\0' || n < 0)
        return -1;
    return (int)n;
}

bool mtd_is_nor(const char *dev_path)
{
    int n = mtd_parse_dev_num(dev_path);
    if (n < 0)
        return false;

    char path[64], buf[16];
    snprintf(path, sizeof(path), "/sys/class/mtd/mtd%d/type", n);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    int r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0)
        return false;
    buf[r] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return strcmp(buf, "nor") == 0;
}

int mtd_find_master(bool want_nor)
{
    FILE *fp = fopen(PROC_MTD, "r");
    if (!fp)
        return -1;

    int master = -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int num;
        if (sscanf(line, "mtd%d:", &num) != 1)
            continue;

        char attr[64];
        snprintf(attr, sizeof(attr), "/sys/class/mtd/mtd%d/offset", num);
        if (access(attr, F_OK) == 0)
            continue;           /* has an offset attribute — a partition */

        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/mtd%d", num);
        if (mtd_is_nor(dev) != want_nor)
            continue;

        master = num;
        break;
    }
    fclose(fp);
    return master;
}

int ubi_find_num_by_mtd(int mtd_num)
{
    if (mtd_num < 0)
        return -1;

    char path[64], buf[32];
    for (int i = 0; i < UBI_SEARCH_MAX; i++) {
        snprintf(path, sizeof(path), UBI_SYS_FMT, i);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        if (atoi(buf) == mtd_num)
            return i;
    }
    return -1;
}
