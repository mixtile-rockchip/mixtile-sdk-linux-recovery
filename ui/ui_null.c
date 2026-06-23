/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2024 Rockchip Electronics Co., Ltd.
 *
 * No-UI (headless) mode — all UI calls are no-ops.
 * Used when RK_UI_ENABLE is disabled or no display is connected.
 * Ported from noui.c.
 */

#define __MODULE__ "ui:null"

#include <stdarg.h>
#include <stdio.h>

#include "recovery/ui.h"
#include "recovery/log.h"

void ui_init(void) {}
void ui_set_background(int icon) { (void)icon; }
void ui_show_indeterminate_progress(void) {}
void ui_show_progress(float portion, int seconds) { (void)portion; (void)seconds; }
void ui_set_progress(float fraction) { (void)fraction; }
void ui_reset_progress(void) {}

void ui_print(const char *fmt, ...)
{
    /* In headless mode, forward UI messages to the log */
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pr_info("%s", buf);
}

void ui_start_menu(char **headers, char **items, int initial_selection)
{
    (void)headers; (void)items; (void)initial_selection;
}

int ui_menu_select(int sel)
{
    (void)sel;
    return 0;
}

void ui_end_menu(void) {}

int ui_text_visible(void) { return 0; }
void ui_show_text(int visible) { (void)visible; }

int ui_wait_key(void)
{
    /* Headless mode: block forever (recovery will proceed without input) */
    return 0;
}

int ui_key_pressed(int key)
{
    (void)key;
    return 0;
}

void ui_clear_key_queue(void) {}

void gr_set_rotate(int val) { (void)val; }
