/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#ifndef RK_RECOVERY_UI_H
#define RK_RECOVERY_UI_H

#include <stdbool.h>

/* ---- Background icons ---- */
enum {
    BACKGROUND_ICON_NONE,
    BACKGROUND_ICON_INSTALLING,
    BACKGROUND_ICON_ERROR,
    NUM_BACKGROUND_ICONS,
};

/* ---- UI core (ui/ui.c or ui/ui_null.c) ---- */
void ui_init(void);
void ui_set_background(int icon);
void ui_show_indeterminate_progress(void);
void ui_show_progress(float portion, int seconds);
void ui_set_progress(float fraction);
void ui_reset_progress(void);
void ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_start_menu(char **headers, char **items, int initial_selection);
int  ui_menu_select(int sel);
void ui_end_menu(void);
int  ui_text_visible(void);
void ui_show_text(int visible);
int  ui_wait_key(void);
int  ui_key_pressed(int key);
void ui_clear_key_queue(void);

/* ---- Device callbacks (ui/menu.c) ---- */
#define NO_ACTION       (-1)
#define HIGHLIGHT_UP    (-2)
#define HIGHLIGHT_DOWN  (-3)
#define SELECT_ITEM     (-4)

#define ITEM_REBOOT             0
#define ITEM_APPLY_SDCARD       1
#define ITEM_APPLY_USERDATA     2
#define ITEM_APPLY_UDISK        3
#define ITEM_WIPE_DATA          4

int device_recovery_start(void);
int device_toggle_display(volatile char *key_pressed, int key_code);
int device_reboot_now(volatile char *key_pressed, int key_code);
int device_handle_key(int key_code, int visible);
int device_perform_action(int which);
int device_wipe_data(void);

extern char *MENU_HEADERS[];
extern char *MENU_ITEMS[];

#endif /* RK_RECOVERY_UI_H */
