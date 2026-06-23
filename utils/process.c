/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "process"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "recovery/process.h"
#include "recovery/log.h"

int rk_run_cmd(const char *prog, char *const argv[])
{
    if (!prog || !argv || !argv[0]) {
        errno = EINVAL;
        pr_err("invalid arguments\n");
        return -1;
    }

    struct sigaction sa_ign = {0}, old_sigint = {0}, old_sigquit = {0};
    sigset_t block_chld, old_mask;

    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sigaction(SIGINT,  &sa_ign, &old_sigint);
    sigaction(SIGQUIT, &sa_ign, &old_sigquit);

    sigemptyset(&block_chld);
    sigaddset(&block_chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block_chld, &old_mask);

    pid_t pid = fork();

    if (pid < 0) {
        int saved = errno;
        pr_err("fork failed for '%s': %s\n", prog, strerror(errno));
        sigaction(SIGINT,  &old_sigint,  NULL);
        sigaction(SIGQUIT, &old_sigquit, NULL);
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        errno = saved;
        return -1;
    }

    if (pid == 0) {
        struct sigaction sa_dfl = {0};
        sa_dfl.sa_handler = SIG_DFL;
        sigemptyset(&sa_dfl.sa_mask);
        sigaction(SIGINT,  &sa_dfl, NULL);
        sigaction(SIGQUIT, &sa_dfl, NULL);
        sigprocmask(SIG_SETMASK, &old_mask, NULL);

        setpgid(0, 0);

        execvp(prog, argv);

        pr_err("execvp '%s' failed: %s\n", prog, strerror(errno));
        _exit(127);
    }

    setpgid(pid, pid);

    int status = 0;
    pid_t ret;
    do {
        ret = waitpid(pid, &status, 0);
    } while (ret == -1 && errno == EINTR);

    sigaction(SIGINT,  &old_sigint,  NULL);
    sigaction(SIGQUIT, &old_sigquit, NULL);
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    if (ret == -1) {
        pr_err("waitpid failed for '%s': %s\n", prog, strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0)
            pr_err("'%s' exited with code %d\n", prog, code);
        return code;
    }

    if (WIFSIGNALED(status)) {
        pr_err("'%s' killed by signal %d (%s)\n",
               prog, WTERMSIG(status), strsignal(WTERMSIG(status)));
        return -2;
    }

    return -1;
}
