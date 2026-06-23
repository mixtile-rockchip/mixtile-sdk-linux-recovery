/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "download"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

#include "recovery/config.h"
#include "recovery/download.h"
#include "recovery/log.h"

#if defined(RK_SOURCE_URL)
#include <curl/curl.h>

static size_t write_callback(void *ptr, size_t size, size_t nmemb,
                             FILE *stream)
{
    return fwrite(ptr, size, nmemb, stream);
}

static int progress_callback(void *clientp, double dltotal, double dlnow,
                              double ultotal, double ulnow)
{
    (void)clientp;
    (void)ultotal;
    (void)ulnow;

    if (dltotal > 0) {
        int pct = (int)(dlnow / dltotal * 100);
        static int last_pct = -1;
        if (pct != last_pct) {
            last_pct = pct;
            pr_info("download: %d%%\n", pct);
        }
    }
    return 0;
}

static int download_http(const char *url, const char *output_path)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        pr_err("curl_easy_init failed\n");
        return -1;
    }

    FILE *outfile = fopen(output_path, "wb");
    if (!outfile) {
        pr_err("cannot create %s: %s\n", output_path, strerror(errno));
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, outfile);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);

    CURLcode res = curl_easy_perform(curl);
    fclose(outfile);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        pr_err("download failed: %s\n", curl_easy_strerror(res));
        return -1;
    }
    pr_info("download complete: %s\n", output_path);
    return 0;
}
#endif /* RK_SOURCE_URL */

/*
 * Copy a regular file with progress.
 *
 * Used when image_url is a local path (USB, SD, mounted share) and the
 * caller asked for a different save_path. Reasons we copy rather than
 * read the source in place:
 *   - Two-step upgrade: BCB points at save_path; recovery on the next
 *     boot must find the image there even if the USB stick is gone.
 *   - The user may eject the source mid-flash if reads go straight to it.
 *   - Source (USB 2.0 ~30 MB/s) is often slower than destination
 *     (SATA/eMMC 200+ MB/s); copy once, amortise the I/O.
 */
static int file_copy(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        pr_err("copy open %s: %s\n", src, strerror(errno));
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        pr_err("copy open %s: %s\n", dst, strerror(errno));
        close(in);
        return -1;
    }

    off_t total = lseek(in, 0, SEEK_END);
    lseek(in, 0, SEEK_SET);

    uint8_t buf[64 * 1024];
    off_t done = 0;
    const off_t PROGRESS_STEP = 64LL << 20;
    off_t next_progress = (total >= PROGRESS_STEP) ? PROGRESS_STEP : -1;

    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write(out, buf, (size_t)n);
        if (w != n) {
            pr_err("copy write short (%zd/%zd) to %s: %s\n",
                   w, n, dst, strerror(errno));
            close(in); close(out);
            return -1;
        }
        done += n;
        if (next_progress > 0 && done >= next_progress) {
            pr_info("copy progress: %lld / %lld MiB (%s -> %s)\n",
                    (long long)(done >> 20), (long long)(total >> 20),
                    src, dst);
            next_progress += PROGRESS_STEP;
        }
    }
    if (n < 0) {
        pr_err("copy read %s: %s\n", src, strerror(errno));
        close(in); close(out);
        return -1;
    }

    if (fsync(out) != 0)
        pr_warn("copy fsync %s: %s\n", dst, strerror(errno));
    close(in);
    close(out);
    pr_info("copy done: %lld MiB %s -> %s\n",
            (long long)(done >> 20), src, dst);
    return 0;
}

int download_file(const char *url, const char *save_path)
{
    if (!url || !url[0]) {
        pr_err("download: url is empty\n");
        return -1;
    }

    /* No save_path or same path: use image in place. */
    if (!save_path || !save_path[0] || strcmp(url, save_path) == 0) {
        pr_info("image used in place: %s\n", url);
        return 0;
    }

    if (strncmp(url, "http", 4) == 0) {
#if defined(RK_SOURCE_URL)
        pr_info("downloading %s -> %s\n", url, save_path);
        return download_http(url, save_path);
#else
        pr_err("HTTP url given but RK_SOURCE_URL not enabled: %s\n", url);
        return -1;
#endif
    }

    pr_info("staging %s -> %s\n", url, save_path);
    return file_copy(url, save_path);
}
