/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "recovery:cmd"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libgen.h>
#include <limits.h>

#include "recovery/bootmsg.h"
#include "recovery/volume.h"
#include "recovery/update_source.h"
#include "recovery/config.h"
#include "recovery/log.h"
#include "recovery_local.h"
#if defined(RK_UI_ENABLE)
#include "recovery/ui.h"
#endif

#define MAX_ARGS        100
#define MAX_ARG_LENGTH  4096
#define URL_MAX_LENGTH  512

#define COMMAND_FILE    "/userdata/recovery/command"
#define INTENT_FILE     "/userdata/recovery/intent"
#define LOG_FILE        "/userdata/recovery/log"
#define LAST_LOG_FILE   "/userdata/recovery/last_log"

static long tmplog_offset = 0;

/* ---- Helper: open file, mounting partition if needed ---- */
static FILE *fopen_path(const char *path, const char *mode)
{
    if (ensure_path_mounted(path) != 0) {
        pr_err("can't mount %s\n", path);
        return NULL;
    }

    /* Create parent directory if writing */
    if (mode[0] == 'w' || mode[0] == 'a') {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", path);
        /* Find last slash and create dir up to it */
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(dir, 0777);
        }
    }

    return fopen(path, mode);
}

/* ---- Copy recovery log to persistent storage ---- */
static void copy_log_file(const char *destination, int append)
{
    /* If RK_RECOVERY_LOG was never created (open failed, e.g. tmpfs full),
     * silent skip rather than pr_err which spams every finish_recovery call. */
    if (access(RK_RECOVERY_LOG, F_OK) != 0)
        return;

    FILE *log = fopen_path(destination, append ? "a" : "w");
    if (log == NULL) {
        pr_err("can't open %s\n", destination);
        return;
    }

    FILE *tmplog = fopen(RK_RECOVERY_LOG, "r");
    if (tmplog == NULL) {
        pr_err("can't open %s\n", RK_RECOVERY_LOG);
        fclose(log);
        return;
    }

    if (append)
        fseek(tmplog, tmplog_offset, SEEK_SET);

    char buf[4096];
    while (fgets(buf, sizeof(buf), tmplog))
        fputs(buf, log);

    if (append)
        tmplog_offset = ftell(tmplog);

    fclose(tmplog);
    fclose(log);
}

/* ---- Public: parse BCB + command file for recovery arguments ---- */
void get_args(int *argc, char ***argv, struct storage_mgr *mgr)
{
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    bcb_read(mgr, &boot);

    if (boot.command[0] != 0 && boot.command[0] != (char)255)
        pr_info("boot command: %.*s\n", (int)sizeof(boot.command),
                boot.command);

    if (boot.status[0] != 0 && boot.status[0] != (char)255)
        pr_info("boot status: %.*s\n", (int)sizeof(boot.status),
                boot.status);

    /* Try BCB recovery field */
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && strcmp(arg, "recovery") == 0) {
            char *argv0 = (*argv && *argc > 0) ? (*argv)[0] : NULL;
            char **new_argv = malloc(sizeof(char *) * MAX_ARGS);
            if (!new_argv) {
                pr_err("get_args: OOM (BCB branch)\n");
                return;
            }
            *argv = new_argv;
            (*argv)[0] = argv0 ? argv0 : strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                arg = strtok(NULL, "\n");
                if (arg == NULL)
                    break;
                (*argv)[*argc] = strdup(arg);
            }
            pr_info("got arguments from BCB\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != (char)255) {
            pr_err("bad boot message: \"%.20s\"\n", boot.recovery);
        }
    }

    /* Fall back to command file */
    if (*argc <= 1) {
        FILE *fp = fopen_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char **new_argv = malloc(sizeof(char *) * MAX_ARGS);
            if (!new_argv) {
                pr_err("get_args: OOM (file branch)\n");
                fclose(fp);
                return;
            }
            char *argv0 = (*argv)[0];
            *argv = new_argv;
            (*argv)[0] = argv0;

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp))
                    break;
                char *nl = strtok(buf, "\r\n");
                if (nl)
                    (*argv)[*argc] = strdup(nl);
            }
            fclose(fp);
            pr_info("got arguments from %s\n", COMMAND_FILE);
        }
    }

    /* Write args back to BCB so recovery restarts on crash */
    extern size_t strlcpy(char *, const char *, size_t);
    extern size_t strlcat(char *, const char *, size_t);

    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    for (int i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    bcb_write(mgr, &boot);
}

/* ---- Public: clear BCB and save logs ---- */
void finish_recovery(struct storage_mgr *mgr, const char *send_intent)
{
    if (send_intent != NULL) {
        FILE *fp = fopen_path(INTENT_FILE, "w");
        if (fp != NULL) {
            fputs(send_intent, fp);
            fclose(fp);
        }
    }

    pr_info("finish_recovery\n");

    /* Copy logs to persistent storage */
    copy_log_file(LOG_FILE, 1);
    copy_log_file(LAST_LOG_FILE, 0);
    chmod(LAST_LOG_FILE, 0640);

    /* Clear BCB to prevent recovery loop. Upgrade success/failure is no
     * longer stashed in BCB — it lives in the recovery log instead. */
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    bcb_write(mgr, &boot);

    /* Remove command file */
    if (ensure_path_mounted(COMMAND_FILE) == 0)
        unlink(COMMAND_FILE);

    sync();
}

/* ---- Internal: run updateEngine child and parse pipe output ---- */
static int start_updateEngine(const char *binary, char *args[], int *pipefd)
{
    pid_t pid = fork();
    if (pid < 0) {
        pr_err("fork updateEngine failed: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        execv(binary, args);
        pr_err("can't run %s (%s)\n", binary, strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);

    char buffer[1024];
    FILE *from_child = fdopen(pipefd[0], "r");
    if (!from_child) {
        pr_err("fdopen updateEngine pipe failed: %s\n", strerror(errno));
        close(pipefd[0]);
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }

    int read_error = 0;
    while (fgets(buffer, sizeof(buffer), from_child) != NULL) {
        char *command = strtok(buffer, " \n");
        if (command == NULL)
            continue;

        if (strcmp(command, "progress") == 0) {
            /* progress <fraction> <seconds> — UI progress (Phase 8) */
            pr_dbg("progress update from updateEngine\n");
        } else if (strcmp(command, "set_progress") == 0) {
            pr_dbg("set_progress from updateEngine\n");
        } else if (strcmp(command, "ui_print") == 0) {
            char *str = strtok(NULL, "\n");
            if (str) {
                pr_info("updateEngine: %s\n", str);
#if defined(RK_UI_ENABLE)
                ui_print("%s\n", str);
#endif
            }
        }
    }
    if (ferror(from_child)) {
        pr_err("read updateEngine pipe failed\n");
        read_error = 1;
    }

    fclose(from_child);

    int status;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        pr_err("waitpid updateEngine failed: %s\n", strerror(errno));
        return -1;
    }

    if (read_error)
        return -1;

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) {
            pr_err("updateEngine exited with status %d\n", code);
#if defined(RK_UI_ENABLE)
            ui_print("Update FAILED.\n");
#endif
            return -1;
        }
        return 0;
    }
    if (WIFSIGNALED(status))
        pr_err("updateEngine killed by signal %d\n", WTERMSIG(status));
    else
        pr_err("updateEngine exited abnormally (status=0x%x)\n", status);
    return -1;
}

/* ---- Public: launch updateEngine with proper arguments ---- */
int do_rk_updateEngine(const char *image_path,
                       enum update_src_type src_type,
                       const char *upgrade_list)
{
    pr_info("launching updateEngine for %s\n", image_path);

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        pr_err("pipe() failed\n");
        return -1;
    }

    const char *binary = "/usr/bin/updateEngine";
    char args_pipefd[32];
    char args_image[URL_MAX_LENGTH];
    char args_upgrade[256];

    snprintf(args_pipefd, sizeof(args_pipefd), "--pipefd=%d", pipefd[1]);
    snprintf(args_image, sizeof(args_image), "--image_url=%s", image_path);

    /*
     * Image candidate priority (update_ab > update_ota > sdupdate.img) is
     * now resolved by source_get_image_path() in source.c. recovery_main
     * passes us the already-resolved image_path; we just forward it.
     */

    const char *update_flag;
    if (src_type == SRC_SDCARD)
        update_flag = "--update=sdboot";
    else if (src_type == SRC_USB)
        update_flag = "--update=usbboot";
    else
        update_flag = "--update";

    /*
     * WHAT to upgrade. SD/USB boot is a full-disk re-image and MUST be
     * complete: udev/by-name may be absent, uboot may pass no mtdparts, the
     * board may be virgin, and the GPT itself is being rewritten — so the
     * whole image (including parameter/bootloader, which "all" alone
     * excludes) is flashed via bare_dev + per-item flash_offset.
     * OTA (SRC_LOCAL) trusts the caller's explicit list (forwarded from BCB
     * recovery args); refuse if absent — no implicit "upgrade everything".
     */
    if (src_type == SRC_SDCARD || src_type == SRC_USB) {
        snprintf(args_upgrade, sizeof(args_upgrade),
                 "--upgrade=all,parameter,bootloader");
    } else {
        if (!upgrade_list || !upgrade_list[0]) {
            pr_err("OTA update requires --upgrade=<list>\n");
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }
        snprintf(args_upgrade, sizeof(args_upgrade), "--upgrade=%s", upgrade_list);
    }

    /* Build args array */
    char *args[8];
    int n = 0;
    args[n++] = (char *)binary;
    args[n++] = args_pipefd;
    args[n++] = args_image;
    args[n++] = (char *)update_flag;
    args[n++] = args_upgrade;
    args[n] = NULL;

    pr_info("updateEngine args: %s %s %s %s\n",
            args[1], args[2], args[3], args[4]);

    return start_updateEngine(binary, args, pipefd);
}

/*
 * Post-upgrade flow shared by SD-boot and USB-boot paths. Both modes use
 * the same model: wait for the user to physically remove the boot media
 * (mandatory — rebooting with media inserted loops straight back into
 * SD/USB recovery; with a failed upgrade that's an infinite death loop),
 * then either fall through to a normal reboot on success, or on failure
 * drop into the UI menu (if available) or halt (if not) so the user can
 * see what happened.
 */
static void handle_media_post_upgrade(struct storage_mgr *mgr,
                                      const char *send_intent,
                                      int status,
                                      const char *mount_point,
                                      const char *media_label)
{
    /* Snapshot the device path before umount so we can poll its existence
     * to detect physical removal. */
    const char *media_dev = get_mounted_device_from_path(mount_point);
    char *media_dev_copy = media_dev ? strdup(media_dev) : NULL;
    ensure_ex_path_unmounted(mount_point);

    /* Dual writer is always on, so pr_info / pr_err naturally reach both
     * the serial console and the log file — no special handling needed. */
    if (status == 0) {
        pr_info("Update SUCCESS. Please remove %s! Waiting for reboot.\n",
                media_label);
#if defined(RK_UI_ENABLE)
        ui_print("Please remove %s!\n", media_label);
        ui_print("Waiting for reboot...\n");
#endif
    } else {
        pr_err("Update FAILED (status=%d). Please remove %s!\n",
               status, media_label);
#if defined(RK_UI_ENABLE)
        ui_print("Please remove %s!\n", media_label);
#endif
    }

    if (media_dev_copy) {
        while (access(media_dev_copy, F_OK) == 0)
            sleep(1);
        free(media_dev_copy);
    }

    if (status == 0)
        return;     /* success → caller falls through to finish_recovery + reboot */

    /* Failure path: clear BCB so any user-driven reboot from the menu goes
     * to normal boot rather than back into recovery. */
    finish_recovery(mgr, send_intent);

#if defined(RK_UI_ENABLE)
    if (ui_text_visible()) {
        pr_info("upgrade failed, entering recovery menu\n");
        prompt_and_wait();
        return;     /* menu returns → fall through to the reboot at end of main */
    }
#endif

    pr_err("upgrade failed and no UI available; halting recovery — "
           "manual reboot required\n");
    while (1)
        pause();
}

/* ---- Top-level dispatch handlers (called by recovery_main) ---- */

/*
 * OTA update: mount the package's filesystem, free every other mount so the
 * target partitions are writable, then run updateEngine. Returns updateEngine
 * status (0 = success).
 */
int run_ota_update(const struct recovery_args *args)
{
    const char *update_package = args->update_package;

    /* Mount update package with retry */
    int mounted = -1;
    for (int i = 0; i < 5; i++) {
        if (ensure_path_mounted(update_package) == 0) {
            mounted = 0;
            break;
        }
        pr_warn("mount %s failed, retry %d\n", update_package, i + 1);
        sleep(1);
    }

    if (mounted != 0) {
        pr_err("mount %s failed after retries\n", update_package);
#if defined(RK_UI_ENABLE)
        ui_print("Cannot mount %s.\n", update_package);
#endif
        return -1;
    }

    /* Unmount everything except the package's own filesystem so the flash
     * targets are free. updateEngine's precheck_items mount guard is the
     * second-line check if a busy fd blocks an unmount. */
    char keep[PATH_MAX] = "/";
    mount_point_of(update_package, keep, sizeof(keep));
    umount_all_except(keep);

    pr_info("upgrading from %s\n", update_package);
    /* WHAT to upgrade comes from the --upgrade= recovery arg (set by whoever
     * wrote the BCB / command file; get_args parsed it into upgrade_list).
     * No more BCB needupdate bitmask. */
    int status = do_rk_updateEngine(update_package, SRC_LOCAL,
                                    args->upgrade_list);

    if (status == 0) {
        /* Delete update.img on success */
        if (access(update_package, F_OK) == 0)
            remove(update_package);
        pr_info("update success\n");
    } else {
        pr_err("update failed\n");
    }
    return status;
}

/*
 * SD-card / U-disk boot update: flash via updateEngine, then run the shared
 * media post-flow (wait for removal; on failure UI menu or halt). The SD and
 * USB paths differ only in src / image_path / mount_point / label.
 */
int run_media_update(struct storage_mgr *mgr, const char *send_intent,
                     enum update_src_type src, const char *image_path,
                     const char *mount_point, const char *media_label)
{
    umount_all_except(mount_point);

    int status = do_rk_updateEngine(image_path, src, NULL);
    handle_media_post_upgrade(mgr, send_intent, status, mount_point,
                              media_label);
    return status;
}

/* ---- Data wipe operations (merged from wipe.c) ---- */

int erase_volume(const char *volume)
{
    pr_info("formatting %s...\n", volume);
    int ret = format_volume(volume);
    if (ret != 0)
        pr_err("format %s failed\n", volume);
    else
        pr_info("format %s done\n", volume);
    return ret;
}

int wipe_data(void)
{
    pr_info("wiping userdata...\n");
    if (erase_volume("/userdata") != 0) {
        pr_err("userdata wipe failed\n");
        return -1;
    }
    pr_info("userdata wipe done\n");
    return 0;
}
