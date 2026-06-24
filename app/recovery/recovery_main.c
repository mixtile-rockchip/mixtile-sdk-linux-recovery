/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "recovery"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/time.h>
#include <time.h>

#include "recovery/bootmsg.h"
#include "recovery/volume.h"
#include "recovery/storage.h"
#include "recovery/update_source.h"
#include "recovery/config.h"
#include "recovery/log.h"
#include "recovery_local.h"
#if defined(RK_UI_ENABLE)
#include "recovery/ui.h"
#endif

#define RECOVERY_VERSION "2.0.0"

/* Source detection APIs declared in <recovery/update_source.h>. */

static const struct option recovery_options[] = {
    { "send_intent",    required_argument, NULL, 's' },
    { "update_package", required_argument, NULL, 'u' },
    { "upgrade",        required_argument, NULL, 'g' },
    { "wipe_data",      no_argument,       NULL, 'w' },
    { "wipe_all",       no_argument,       NULL, 'a' },
    { "show_text",      no_argument,       NULL, 't' },
    { NULL,             0,                 NULL,  0  },
};

/* Boot-media detection results, produced by detect_update_sources(). */
struct update_targets {
    bool is_sd_boot;
    bool is_usb_boot;
    bool is_sd_update;
    bool is_usb_update;
    char sd_image_path[128];
    char usb_image_path[128];
};

static void parse_args(int argc, char **argv, struct recovery_args *args)
{
    int arg;
    while ((arg = getopt_long(argc, argv, "", recovery_options, NULL)) != -1) {
        switch (arg) {
        case 's': args->send_intent = optarg; break;
        case 'u': args->update_package = optarg; break;
        case 'g': args->upgrade_list = optarg; break;
        case 'w': args->wipe = true; break;
        case 'a': args->wipe_all = true; break;
        case 't': args->show_text = true; break;  /* show_text — UI Phase 8 */
        case '?': pr_err("invalid argument\n"); break;
        }
    }
}

/*
 * Detect SD/USB boot from kernel cmdline and resolve the update image path.
 * Vendor fstab on rk3588 mis-mounts the boot media (sda1 -> /mnt/udisk fails
 * because SATA takes sda; mmcblk1p1 may not exist), so source_ensure_mounted()
 * discovers and mounts the actual device before checking fw_update / image
 * path. source_get_image_path walks the priority candidate names (update_ab >
 * update_ota > sdupdate.img > update.img) and honors `image=xxx` override.
 */
static void detect_update_sources(struct update_targets *t)
{
#if defined(RK_SOURCE_SD)
    t->is_sd_boot = source_is_boot(SRC_SDCARD);
    if (t->is_sd_boot) {
        source_ensure_mounted(SRC_SDCARD);
        t->is_sd_update = source_has_update(SRC_SDCARD);
    }
#endif

#if defined(RK_SOURCE_USB)
    t->is_usb_boot = source_is_boot(SRC_USB);
    if (t->is_usb_boot) {
        source_ensure_mounted(SRC_USB);
        t->is_usb_update = source_has_update(SRC_USB);
    }
#endif

    if (t->is_sd_update) {
        struct update_source src;
        if (source_detect_active(&src) == 0 && src.type == SRC_SDCARD) {
            if (source_get_image_path(&src, t->sd_image_path,
                                      sizeof(t->sd_image_path)) != 0) {
                pr_warn("SD image not found in any candidate\n");
                t->is_sd_update = false;
            }
        } else {
            t->is_sd_update = false;
        }
    }

    if (t->is_usb_update) {
        /* Build USB source explicitly (active source might be SD; we want
         * USB here regardless because is_usb_boot is already true). */
        struct update_source src;
        if (source_detect_active(&src) == 0 && src.type == SRC_USB) {
            if (source_get_image_path(&src, t->usb_image_path,
                                      sizeof(t->usb_image_path)) != 0) {
                pr_warn("USB image not found in any candidate\n");
                t->is_usb_update = false;
            }
        } else {
            t->is_usb_update = false;
        }
    }
}

int main(int argc, char **argv)
{
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    /* Initialize storage */
    struct storage_mgr *mgr = storage_init();
    if (!mgr) {
        pr_err("storage_init failed\n");
        return -1;
    }

    /* Parse BCB and command file for arguments (may rewrite argc/argv) */
    get_args(&argc, &argv, mgr);

    struct recovery_args args = {0};
    parse_args(argc, argv, &args);

    /*
     * Dual writer (console + RK_RECOVERY_LOG) is the only log routing mode.
     * fopencookie fans every printf/fprintf out to BOTH the original console
     * fd AND the log file fd; synchronous, no subprocess, no risk of losing
     * the tail of the log on reboot. finish_recovery later copies
     * RK_RECOVERY_LOG to /userdata/recovery/log for post-mortem.
     */
    rk_log_set_level(LOG_DEBUG);
    rk_log_install_dual_writer(RK_RECOVERY_LOG);

    printf("\n");
    printf("*********************************************************\n");
    printf("            ROCKCHIP recovery v%s\n", RECOVERY_VERSION);
    printf("*********************************************************\n");

    time_t start = time(NULL);
    pr_info("starting recovery on %s", ctime(&start));

    /* Wait for coldboot completion */
    int coldboot_wait = 0;
    while (access("/dev/.coldboot_done", F_OK) != 0 && coldboot_wait < 30) {
        pr_info("coldboot not done, wait...\n");
        sleep(1);
        coldboot_wait++;
    }

#if defined(RK_UI_ENABLE)
    ui_init();
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_text(1);
#endif

    /* Load filesystem table */
    load_volume_table();

    struct update_targets t = {0};
    detect_update_sources(&t);

    /* Log parsed command */
    pr_info("command:");
    for (int i = 0; i < argc; i++)
        printf(" \"%s\"", argv[i]);
    printf("\n");

    /* ---- Command dispatch ---- */
    int status = 0;     /* 0 = success */

    if (args.update_package != NULL) {
        status = run_ota_update(&args);
    } else if (t.is_sd_update) {
        status = run_media_update(mgr, args.send_intent, SRC_SDCARD,
                                  t.sd_image_path, RK_SD_MOUNT_POINT, "SD CARD");
    } else if (t.is_usb_update) {
        status = run_media_update(mgr, args.send_intent, SRC_USB,
                                  t.usb_image_path, RK_USB_MOUNT_POINT, "U DISK");
    } else if (args.wipe || args.wipe_all) {
        if (wipe_data() != 0) {
            pr_err("data wipe failed\n");
            status = -1;
        }
    } else {
        /*
         * No command: idle mode. With a visible UI on a fixed-rootfs device,
         * drop into the menu so the user can pick reboot / wipe / apply
         * update. SD/USB boot paths have their own remove-media-then-reboot
         * flow inside run_media_update.
         */
        if (argc <= 1 && !t.is_sd_boot && !t.is_usb_boot)
            pr_info("no command specified, entering menu\n");
    }

    /* Cleanup first: clear BCB so a user-picked reboot from the prompt goes
     * back to normal instead of looping into recovery. legacy
     * (recovery.c:1297-1300) and AOSP do the same. */
    finish_recovery(mgr, args.send_intent);

#if defined(RK_UI_ENABLE)
    /*
     * AOSP-style failure path: clear BCB (above), then show the error menu
     * and wait for user. SD/USB boot upgrades skip the menu — those typically
     * fail due to media issues and the standard "remove SD/USB → reboot" flow
     * is more useful.
     */
    bool fallback_menu =
        (status != 0 || (argc <= 1 && !t.is_sd_boot && !t.is_usb_boot))
        && !t.is_sd_boot && !t.is_usb_boot
        && ui_text_visible();

    if (status != 0)
        pr_err("operation failed (status=%d), entering prompt_and_wait\n", status);

    if (fallback_menu)
        prompt_and_wait();
#else
    if (status != 0)
        pr_err("operation failed (status=%d)\n", status);
#endif

    gettimeofday(&end_time, NULL);
    long long elapsed = (end_time.tv_sec - start_time.tv_sec) * 1000LL +
                        (end_time.tv_usec - start_time.tv_usec) / 1000LL;
    pr_info("recovery completed in %lld ms\n", elapsed);

    pr_info("rebooting...\n");
    fflush(stdout);
    sync();
    reboot(RB_AUTOBOOT);
    return 0;
}
