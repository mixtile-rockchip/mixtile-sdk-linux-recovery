/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (C) 2024 Rockchip Electronics Co., Ltd.
 *
 * Default recovery menu and device callbacks.
 * Ported from default_recovery_ui.c.
 */

#include <stddef.h>
#include <linux/input.h>

#include "recovery/ui.h"

char *MENU_HEADERS[] = {
    "Linux system recovery utility",
    "",
    NULL,
};

char *MENU_ITEMS[] = {
    "reboot system now",
    "apply update from sdcard",
    "apply update from local userdata",
    "apply update from udisk",
    "wipe data/factory reset",
    NULL,
};

int device_recovery_start(void)
{
    return 0;
}

int device_toggle_display(volatile char *key_pressed, int key_code)
{
    (void)key_pressed;
    (void)key_code;
    return 0;
}

int device_reboot_now(volatile char *key_pressed, int key_code)
{
    (void)key_pressed;
    (void)key_code;
    return 0;
}

int device_handle_key(int key_code, int visible)
{
    if (visible) {
        switch (key_code) {
        case KEY_DOWN:
        case KEY_VOLUMEDOWN:
            return HIGHLIGHT_DOWN;

        case KEY_UP:
        case KEY_VOLUMEUP:
            return HIGHLIGHT_UP;

        case KEY_ENTER:
        case KEY_MENU:
            return SELECT_ITEM;
        }
    }

    return NO_ACTION;
}

int device_perform_action(int which)
{
    return which;
}

int device_wipe_data(void)
{
    return 0;
}
