/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "engine"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/reboot.h>

#include "recovery/storage.h"
#include "recovery/log.h"
#include "recovery/config.h"
#include "cmd.h"

#define ENGINE_VERSION "2.0.0"

static void display_usage(void)
{
    printf("updateEngine v%s\n\n", ENGINE_VERSION);
    printf("Usage:\n");
    printf("  --update [sdboot|usbboot]  Upgrade firmware\n");
    printf("  --image_url=<path>         Path to firmware image\n");
    printf("  --upgrade=all|name,name    Partitions to upgrade (required for update)\n");
    printf("  --savepath=<path>          Save downloaded image to path\n");
    printf("  --misc=now                 Mark current slot successful\n");
    printf("  --misc=other               Switch to other A/B slot\n");
    printf("  --misc=update              OTA update via MISC/BCB\n");
    printf("  --misc=display             Display MISC/A/B info\n");
    printf("  --misc=wipe_userdata       Format data partition\n");
    printf("  --misc_custom=read|write|clean  Custom MISC cmdline\n");
    printf("  --reboot                   Reboot after upgrade\n");
    printf("  --version                  Show version\n");
}

static const struct option engine_options[] = {
    { "update",      optional_argument, NULL, 'u' },
    { "version_url", required_argument, NULL, 'V' },
    { "image_url",   required_argument, NULL, 'I' },
    { "check",       required_argument, NULL, 'c' },
    { "misc",        required_argument, NULL, 'm' },
    { "misc_custom", required_argument, NULL, 'd' },
    { "upgrade",     required_argument, NULL, 'g' },
    { "reboot",      no_argument,       NULL, 'r' },
    { "help",        no_argument,       NULL, 'h' },
    { "pipefd",      required_argument, NULL, 'f' },
    { "savepath",    required_argument, NULL, 's' },
    { "version",     no_argument,       NULL, 'v' },
    { NULL,          0,                 NULL,  0  },
};

/*
 * Parse argv into engine_args. Returns -1 to continue into main's dispatch,
 * or a >= 0 exit code when an option already handled everything (-v / -h).
 */
static int parse_args(int argc, char **argv, struct engine_args *args)
{
    int arg;
    while ((arg = getopt_long(argc, argv, "", engine_options, NULL)) != -1) {
        switch (arg) {
        case 'u':
            args->is_update = true;
            if (optarg) {
                if (strstr(optarg, "sdboot"))
                    args->src_type = SRC_SDCARD;
                else if (strstr(optarg, "usbboot"))
                    args->src_type = SRC_USB;
            }
            break;
        case 'c':  args->version_url = optarg; break;
        case 'm':  args->misc_func = optarg; break;
        case 'g':  args->upgrade_spec = optarg; break;
        case 's':  args->save_path = optarg; break;
        case 'r':  args->is_reboot = true; break;
        case 'V':  args->version_url = optarg; break;
        case 'I':  args->image_url = optarg; break;
        case 'f':  args->pipefd = atoi(optarg); break;
        case 'd':  args->custom_define = optarg; break;
        case 'v':
            printf("updateEngine v%s\n", ENGINE_VERSION);
            return 0;
        case 'h':
            display_usage();
            return 0;
        case '?':
            pr_err("invalid argument\n");
            break;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    pr_info("*** updateEngine v%s ***\n", ENGINE_VERSION);

    struct engine_args args = {0};
    args.src_type = SRC_LOCAL;
    args.pipefd = -1;

    int ec = parse_args(argc, argv, &args);
    if (ec >= 0)
        return ec;

    /*
     * Unconditionally install dual writer (console + RK_RECOVERY_LOG) at
     * DEBUG level. updateEngine runs as a fork+exec child of recovery; the
     * inherited fds 1/2 point at the console, bypassing recovery's own
     * dual writer. Without this, finish_recovery's persisted log would
     * only contain recovery's output, not the upgrade engine's.
     *
     * Both binaries append to the same path; O_APPEND on Linux is atomic
     * per write() for sizes <= PIPE_BUF (4 KiB), so concurrent recovery +
     * updateEngine log lines don't interleave mid-line.
     */
    rk_log_set_level(LOG_DEBUG);
    rk_log_install_dual_writer(RK_RECOVERY_LOG);

    /*
     * cmd_pipe is opened once here and held by main; whichever handler
     * builds an upgrade_context copies it in so status_cb / print_cb can
     * reach it. Misc branches that don't run upgrade_run don't need a pipe.
     * Handlers never close it — main closes it once at the end.
     */
    FILE *cmd_pipe = NULL;
    if (args.pipefd >= 0) {
        cmd_pipe = fdopen(args.pipefd, "wb");
        if (cmd_pipe)
            setlinebuf(cmd_pipe);
    }

    /* Initialize storage once for all commands */
    struct storage_mgr *mgr = storage_init();
    if (!mgr) {
        pr_err("storage_init failed\n");
        if (cmd_pipe)
            fclose(cmd_pipe);
        return -1;
    }

    int rc = -1;
    if (args.is_update)
        rc = cmd_update(mgr, &args, cmd_pipe);
    else if (args.misc_func)
        rc = cmd_misc(mgr, &args, cmd_pipe);
    else if (args.custom_define)
        rc = cmd_misc_custom(mgr, args.custom_define);
    else
        display_usage();

    if (args.is_reboot && rc == 0) {
        sync();
        if (reboot(RB_AUTOBOOT) != 0)
            pr_err("reboot failed: %s\n", strerror(errno));
    }

    if (cmd_pipe)
        fclose(cmd_pipe);

    return rc;
}
