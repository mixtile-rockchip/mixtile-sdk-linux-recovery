/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 * RK parameter.txt partition layout: the mtdparts line parsed into named
 * partitions. Produced by flash_parameter.c (GPT build) and reused by the
 * upgrade layout-closure precheck. offset/size are in 512-sector rk-format
 * units; size == PARAM_SIZE_GROW is the parameter.txt '-@' form ("extend to
 * the end of the disk").
 */
#ifndef RK_RECOVERY_PARAMETER_H
#define RK_RECOVERY_PARAMETER_H

#include <stdint.h>

#define PARAM_SIZE_GROW  0xFFFFFFFFu

struct param_item {
    char        name[20];
    uint32_t    offset;
    uint32_t    size;
};

/*
 * Parse the mtdparts line out of a parameter.txt blob into items[] (up to
 * max). Returns the number parsed. Defined in core/flash_parameter.c (only
 * compiled under RK_GPT_UPDATE).
 */
int gpt_parse_params(const char *param_str, struct param_item *items, int max);

#endif /* RK_RECOVERY_PARAMETER_H */
