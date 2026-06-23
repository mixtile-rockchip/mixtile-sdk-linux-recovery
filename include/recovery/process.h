/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_PROCESS_H
#define RK_RECOVERY_PROCESS_H

/*
 * Run an external command and wait for it to finish.
 *
 * Forks, execvp's prog with argv, and blocks until the child exits.
 * While the child runs, the parent ignores SIGINT/SIGQUIT and blocks
 * SIGCHLD (system(3) semantics); original signal state is restored
 * on return. waitpid is retried on EINTR.
 *
 * prog — absolute path, or a command name resolved via PATH.
 *        execvp() skips PATH lookup when prog contains '/', so both
 *        "/usr/sbin/ubiattach" and "nandwrite" are valid.
 * argv — NULL-terminated argv array; argv[0] is the program name seen
 *        by the child. Must not be NULL and argv[0] must not be NULL.
 *
 * Returns:
 *    0         child exited normally with status 0
 *   >0        child's non-zero exit status
 *   -1        invalid arguments, fork failed, waitpid failed, or child
 *             stopped/continued unexpectedly (errno set where applicable)
 *   -2        child was terminated by a signal (see log for signal number)
 *
 * Note: execvp failure in the child is reported as exit code 127.
 */
int rk_run_cmd(const char *prog, char *const argv[]);

#endif /* RK_RECOVERY_PROCESS_H */
