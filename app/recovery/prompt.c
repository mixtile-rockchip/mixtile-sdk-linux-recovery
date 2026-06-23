/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (C) 2026 Rockchip Electronics Co., Ltd.
 *
 */

#define __MODULE__ "recovery:prompt"

#include "recovery_local.h"
#include "recovery/ui.h"
#include "recovery/update_source.h"
#include "recovery/log.h"

/*
 * Drive the menu key loop until the user makes a selection.
 * Mirrors AOSP recovery's get_menu_selection(): start_menu →
 * wait_key → device_handle_key dispatch → end_menu.
 */
static int get_menu_selection(char **headers, char **items, int initial)
{
    ui_clear_key_queue();
    ui_start_menu(headers, items, initial);
    int selected = initial;
    int chosen = -1;

    while (chosen < 0) {
        int key = ui_wait_key();
        int visible = ui_text_visible();
        int action = device_handle_key(key, visible);

        if (action < 0) {
            switch (action) {
            case HIGHLIGHT_UP:
                selected = ui_menu_select(--selected);
                break;
            case HIGHLIGHT_DOWN:
                selected = ui_menu_select(++selected);
                break;
            case SELECT_ITEM:
                chosen = selected;
                break;
            case NO_ACTION:
            default:
                break;
            }
        } else {
            /* device_handle_key returned a direct ITEM_* code */
            chosen = action;
        }
    }

    ui_end_menu();
    return chosen;
}

/*
 * Try one apply-from-source action. Returns 0 on success (caller should
 * reboot), -1 on failure (caller should loop the menu).
 *
 * Source selection: source_detect_active picks whichever media is
 * present and only honors the user's chosen item if the type matches —
 * e.g., "apply from sdcard" with no SD card returns -1 quickly.
 */
static int try_apply_update(int which)
{
    enum update_src_type want;
    const char *label;
    switch (which) {
    case ITEM_APPLY_SDCARD:    want = SRC_SDCARD; label = "sdcard"; break;
    case ITEM_APPLY_USERDATA:  want = SRC_LOCAL;  label = "userdata"; break;
    case ITEM_APPLY_UDISK:     want = SRC_USB;    label = "udisk"; break;
    default:                   return -1;
    }

    struct update_source src;
    if (source_detect_active(&src) != 0 || src.type != want) {
        ui_print("-- No update.img on %s\n", label);
        return -1;
    }

    char image[256];
    if (source_get_image_path(&src, image, sizeof(image)) != 0) {
        ui_print("-- No update.img on %s\n", label);
        return -1;
    }

    ui_print("-- Installing %s from %s ...\n", image, label);
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();

    /* User-initiated "apply update" from the menu = upgrade everything in
     * the image. Pass "all" so the SRC_LOCAL (userdata) case isn't rejected
     * for a missing --upgrade= (SD/USB force "all" internally regardless). */
    int ret = do_rk_updateEngine(image, src.type, "all");
    ui_reset_progress();

    if (ret == 0) {
        ui_print("-- Install complete.\n");
        return 0;
    }
    ui_print("-- Install failed.\n");
    return -1;
}

int prompt_and_wait(void)
{
    pr_info("entering prompt_and_wait (UI menu)\n");

    for (;;) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_show_text(1);
        ui_reset_progress();

        int chosen = get_menu_selection(MENU_HEADERS, MENU_ITEMS, 0);
        chosen = device_perform_action(chosen);
        pr_info("user picked menu item %d\n", chosen);

        switch (chosen) {
        case ITEM_REBOOT:
            return ITEM_REBOOT;

        case ITEM_WIPE_DATA:
            ui_print("\n-- Wiping userdata...\n");
            if (wipe_data() == 0)
                ui_print("Userdata wipe complete.\n");
            else
                ui_print("Userdata wipe FAILED.\n");
            break;

        case ITEM_APPLY_SDCARD:
        case ITEM_APPLY_USERDATA:
        case ITEM_APPLY_UDISK:
            if (try_apply_update(chosen) == 0)
                return ITEM_REBOOT;     /* success -> reboot into new system */
            break;

        default:
            ui_print("Unknown menu selection: %d\n", chosen);
            break;
        }
    }
}
