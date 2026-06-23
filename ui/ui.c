/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (C) 2024 Rockchip Electronics Co., Ltd.
 *
 * Recovery UI implementation with DRM graphics backend.
 * Ported from ui.c.
 */

#define __MODULE__ "ui"

#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "minui/minui.h"
#include "recovery/ui.h"
#include "recovery/log.h"

#define MAX_COLS 96
#define MAX_ROWS 32

#define CHAR_WIDTH 10
#define CHAR_HEIGHT 18

#define PROGRESSBAR_INDETERMINATE_STATES 6
#define PROGRESSBAR_INDETERMINATE_FPS 15

static pthread_mutex_t gUpdateMutex = PTHREAD_MUTEX_INITIALIZER;
static gr_surface gBackgroundIcon[NUM_BACKGROUND_ICONS];
static gr_surface gProgressBarIndeterminate[PROGRESSBAR_INDETERMINATE_STATES];
static gr_surface gProgressBarEmpty;
static gr_surface gProgressBarFill;

static const struct { gr_surface *surface; const char *name; } BITMAPS[] = {
    { &gBackgroundIcon[BACKGROUND_ICON_INSTALLING], "icon_installing" },
    { &gBackgroundIcon[BACKGROUND_ICON_ERROR],      "icon_error" },
    { &gProgressBarIndeterminate[0],    "indeterminate1" },
    { &gProgressBarIndeterminate[1],    "indeterminate2" },
    { &gProgressBarIndeterminate[2],    "indeterminate3" },
    { &gProgressBarIndeterminate[3],    "indeterminate4" },
    { &gProgressBarIndeterminate[4],    "indeterminate5" },
    { &gProgressBarIndeterminate[5],    "indeterminate6" },
    { &gProgressBarEmpty,               "progress_empty" },
    { &gProgressBarFill,                "progress_fill" },
    { NULL,                             NULL },
};

static gr_surface gCurrentIcon = NULL;

static enum ProgressBarType {
    PROGRESSBAR_TYPE_NONE,
    PROGRESSBAR_TYPE_INDETERMINATE,
    PROGRESSBAR_TYPE_NORMAL,
} gProgressBarType = PROGRESSBAR_TYPE_NONE;

static float gProgressScopeStart = 0, gProgressScopeSize = 0, gProgress = 0;
static time_t gProgressScopeTime, gProgressScopeDuration;

static int gPagesIdentical = 0;

static char text[MAX_ROWS][MAX_COLS];
static int text_cols = 0, text_rows = 0;
static int text_col = 0, text_row = 0, text_top = 0;
static int show_text = 1;

static char menu[MAX_ROWS][MAX_COLS];
static int show_menu = 0;
static int menu_top = 0, menu_items = 0, menu_sel = 0;

static pthread_mutex_t key_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_queue_cond = PTHREAD_COND_INITIALIZER;
static int key_queue[256], key_queue_len = 0;
static volatile char key_pressed[KEY_MAX + 1];

extern GRSurface *gr_draw;

static void draw_background_locked(gr_surface icon)
{
    gPagesIdentical = 0;
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());

    if (icon) {
        int iconWidth = gr_get_width(icon);
        int iconHeight = gr_get_height(icon);
        int iconX = (gr_fb_width() - iconWidth) / 2;
        int iconY = (gr_fb_height() - iconHeight) / 2;
        gr_blit(icon, 0, 0, iconWidth, iconHeight, iconX, iconY);
    }
}

static void draw_progress_locked(void)
{
    if (gProgressBarType == PROGRESSBAR_TYPE_NONE)
        return;

    int iconHeight = gr_get_height(gBackgroundIcon[BACKGROUND_ICON_INSTALLING]);
    int width = gr_get_width(gProgressBarEmpty);
    int height = gr_get_height(gProgressBarEmpty);

    int dx = (gr_fb_width() - width) / 2;
    int dy = (3 * gr_fb_height() + iconHeight - 2 * height) / 4;

    gr_color(0, 0, 0, 255);
    gr_fill(dx, dy, width, height);

    if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL) {
        float progress = gProgressScopeStart + gProgress * gProgressScopeSize;
        int pos = (int)(progress * width);

        if (pos > 0)
            gr_blit(gProgressBarFill, 0, 0, pos, height, dx, dy);
        if (pos < width - 1)
            gr_blit(gProgressBarEmpty, pos, 0, width - pos, height,
                    dx + pos, dy);
    }

    if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE) {
        static int frame = 0;
        gr_blit(gProgressBarIndeterminate[frame], 0, 0, width, height,
                dx, dy);
        frame = (frame + 1) % PROGRESSBAR_INDETERMINATE_STATES;
    }
}

static void draw_text_line(int row, const char *t)
{
    if (t[0] != '\0')
        gr_text(0, (row + 1) * CHAR_HEIGHT - 1, t);
}

static void draw_screen_locked(void)
{
    draw_background_locked(gCurrentIcon);
    draw_progress_locked();

    if (show_text) {
        gr_color(0, 0, 0, 160);
        gr_fill(0, 0, gr_fb_width(), gr_fb_height());

        int i = 0;
        if (show_menu) {
            gr_color(64, 96, 255, 255);
            gr_fill(0, (menu_top + 1 + menu_sel) * CHAR_HEIGHT,
                    gr_fb_width(),
                    (menu_top + menu_sel + 2) * CHAR_HEIGHT + 1);

            for (; i < menu_top + menu_items; ++i) {
                if (i == menu_top + menu_sel) {
                    gr_color(255, 255, 255, 255);
                    draw_text_line(i, menu[i]);
                    gr_color(64, 96, 255, 255);
                } else {
                    draw_text_line(i, menu[i]);
                }
            }
            ++i;
            gr_fill(0, i * CHAR_HEIGHT + CHAR_HEIGHT / 2 - 1,
                    gr_fb_width(), i * CHAR_HEIGHT + CHAR_HEIGHT / 2 + 1);
        }

        gr_color(255, 255, 0, 255);

        for (; i < text_rows; ++i)
            draw_text_line(i, text[(i + text_top) % text_rows]);
    }
}

static void update_screen_locked(void)
{
    draw_screen_locked();
    gr_flip();
}

static void update_progress_locked(void)
{
    if (show_text || !gPagesIdentical) {
        draw_screen_locked();
        gPagesIdentical = 1;
    } else {
        draw_progress_locked();
    }
    gr_flip();
}

static void *progress_thread(void *cookie)
{
    (void)cookie;
    for (;;) {
        usleep(1000000 / PROGRESSBAR_INDETERMINATE_FPS);
        pthread_mutex_lock(&gUpdateMutex);

        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE && !show_text)
            update_progress_locked();

        int duration = gProgressScopeDuration;
        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && duration > 0) {
            int elapsed = time(NULL) - gProgressScopeTime;
            float progress = 1.0 * elapsed / duration;
            if (progress > 1.0)
                progress = 1.0;
            if (progress > gProgress) {
                gProgress = progress;
                update_progress_locked();
            }
        }

        pthread_mutex_unlock(&gUpdateMutex);
    }
    return NULL;
}

static int rel_sum = 0;

static int input_callback(int fd, short revents, void *data)
{
    struct input_event ev;
    int ret;
    int fake_key = 0;

    (void)data;
    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;

    if (ev.type == EV_SYN) {
        return 0;
    } else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
            rel_sum += ev.value;
            if (rel_sum > 3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_DOWN;
                ev.value = 1;
                rel_sum = 0;
            } else if (rel_sum < -3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_UP;
                ev.value = 1;
                rel_sum = 0;
            }
        }
    } else {
        rel_sum = 0;
    }

    if (ev.type != EV_KEY || ev.code > KEY_MAX)
        return 0;

    pthread_mutex_lock(&key_queue_mutex);
    if (!fake_key)
        key_pressed[ev.code] = ev.value;
    const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
    if (ev.value > 0 && key_queue_len < queue_max) {
        key_queue[key_queue_len++] = ev.code;
        pthread_cond_signal(&key_queue_cond);
    }
    pthread_mutex_unlock(&key_queue_mutex);

    if (ev.value > 0 && device_toggle_display(key_pressed, ev.code)) {
        pthread_mutex_lock(&gUpdateMutex);
        show_text = !show_text;
        update_screen_locked();
        pthread_mutex_unlock(&gUpdateMutex);
    }

    if (ev.value > 0 && device_reboot_now(key_pressed, ev.code))
        reboot(RB_AUTOBOOT);

    return 0;
}

static void *input_thread(void *cookie)
{
    (void)cookie;
    for (;;) {
        if (!ev_wait(-1))
            ev_dispatch();
    }
    return NULL;
}

void ui_init(void)
{
    int ret = gr_init();
    if (ret) {
        pr_err("gr_init failed (%d), running headless\n", ret);
        return;
    }

    ev_init((ev_callback)input_callback, NULL);

    text_col = text_row = 0;
    text_rows = gr_fb_height() / CHAR_HEIGHT;
    if (text_rows > MAX_ROWS)
        text_rows = MAX_ROWS;
    text_top = 1;

    text_cols = gr_fb_width() / CHAR_WIDTH;
    if (text_cols > MAX_COLS - 1)
        text_cols = MAX_COLS - 1;

    for (int i = 0; BITMAPS[i].name != NULL; ++i) {
        int result = res_create_display_surface(BITMAPS[i].name,
                                                BITMAPS[i].surface);
        if (result < 0) {
            if (result == -2)
                pr_info("bitmap %s missing header\n", BITMAPS[i].name);
            else
                pr_err("missing bitmap %s (code %d)\n",
                       BITMAPS[i].name, result);
            *BITMAPS[i].surface = NULL;
        }
    }

    pthread_t t;
    pthread_create(&t, NULL, progress_thread, NULL);
    pthread_create(&t, NULL, input_thread, NULL);
}

void ui_set_background(int icon)
{
    if (!gr_draw)
        return;
    pthread_mutex_lock(&gUpdateMutex);
    gCurrentIcon = gBackgroundIcon[icon];
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_indeterminate_progress(void)
{
    if (!gr_draw)
        return;
    pthread_mutex_lock(&gUpdateMutex);
    if (gProgressBarType != PROGRESSBAR_TYPE_INDETERMINATE) {
        gProgressBarType = PROGRESSBAR_TYPE_INDETERMINATE;
        update_progress_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_progress(float portion, int seconds)
{
    if (!gr_draw)
        return;
    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NORMAL;
    gProgressScopeStart += gProgressScopeSize;
    gProgressScopeSize = portion;
    gProgressScopeTime = time(NULL);
    gProgressScopeDuration = seconds;
    gProgress = 0;
    update_progress_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_set_progress(float fraction)
{
    if (!gr_draw)
        return;
    pthread_mutex_lock(&gUpdateMutex);
    if (fraction < 0.0)
        fraction = 0.0;
    if (fraction > 1.0)
        fraction = 1.0;
    if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL &&
        fraction > gProgress) {
        int width = gr_get_width(gProgressBarIndeterminate[0]);
        float scale = width * gProgressScopeSize;
        if ((int)(gProgress * scale) != (int)(fraction * scale)) {
            gProgress = fraction;
            update_progress_locked();
        }
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_reset_progress(void)
{
    if (!gr_draw)
        return;
    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NONE;
    gProgressScopeStart = gProgressScopeSize = 0;
    gProgressScopeTime = gProgressScopeDuration = 0;
    gProgress = 0;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_print(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (!gr_draw)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        char *ptr;
        for (ptr = buf; *ptr != '\0'; ++ptr) {
            if (*ptr == '\n' || text_col >= text_cols) {
                text[text_row][text_col] = '\0';
                text_col = 0;
                text_row = (text_row + 1) % text_rows;
                if (text_row == text_top)
                    text_top = (text_top + 1) % text_rows;
            }
            if (*ptr != '\n')
                text[text_row][text_col++] = *ptr;
        }
        text[text_row][text_col] = '\0';
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_start_menu(char **headers, char **items, int initial_selection)
{
    int i;

    if (!gr_draw)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        for (i = 0; i < text_rows; ++i) {
            if (headers[i] == NULL)
                break;
            strncpy(menu[i], headers[i], text_cols - 1);
            menu[i][text_cols - 1] = '\0';
        }
        menu_top = i;
        for (; i < text_rows; ++i) {
            if (items[i - menu_top] == NULL)
                break;
            strncpy(menu[i], items[i - menu_top], text_cols - 1);
            menu[i][text_cols - 1] = '\0';
        }
        menu_items = i - menu_top;
        show_menu = 1;
        menu_sel = initial_selection;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_menu_select(int sel)
{
    int old_sel;

    if (!gr_draw)
        return 0;

    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0) {
        old_sel = menu_sel;
        menu_sel = sel;
        if (menu_sel < 0)
            menu_sel = 0;
        if (menu_sel >= menu_items)
            menu_sel = menu_items - 1;
        sel = menu_sel;
        if (menu_sel != old_sel)
            update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
    return sel;
}

void ui_end_menu(void)
{
    if (!gr_draw)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0 && text_rows > 0 && text_cols > 0) {
        show_menu = 0;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_text_visible(void)
{
    if (!gr_draw)
        return 0;

    pthread_mutex_lock(&gUpdateMutex);
    int visible = show_text;
    pthread_mutex_unlock(&gUpdateMutex);
    return visible;
}

void ui_show_text(int visible)
{
    if (!gr_draw)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    show_text = visible;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_wait_key(void)
{
    if (!gr_draw)
        return 0;

    pthread_mutex_lock(&key_queue_mutex);
    while (key_queue_len == 0)
        pthread_cond_wait(&key_queue_cond, &key_queue_mutex);

    int key = key_queue[0];
    memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    pthread_mutex_unlock(&key_queue_mutex);
    return key;
}

int ui_key_pressed(int key)
{
    return key_pressed[key];
}

void ui_clear_key_queue(void)
{
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}
