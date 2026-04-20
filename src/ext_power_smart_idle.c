/*
 * Copyright (c) 2026 Frosthaven
 * SPDX-License-Identifier: MIT
 *
 * Smart ext-power idle module for ZMK.
 * Skips ext-power idle timeout when USB is connected (charging).
 * On battery, auto-offs ext-power after ZMK idle timeout and restores on keypress.
 * Optionally clamps RGB brightness when on battery to reduce power draw.
 * Toggles the ext-power GPIO pin directly to avoid flash writes on auto cycles.
 * Checks ext_power saved state to respect manual toggles (e.g. RGB_TOG, EP_TOG).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zmk/activity.h>
#include <zmk/usb.h>
#include <drivers/ext_power.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT)
#include <zmk/rgb_underglow.h>
#endif

#define EXT_POWER_NODE DT_NODELABEL(ext_power)

static const struct gpio_dt_spec ext_power_gpio =
    GPIO_DT_SPEC_GET(EXT_POWER_NODE, control_gpios);

static const struct device *ext_power_dev = DEVICE_DT_GET(EXT_POWER_NODE);

static bool auto_off_active = false;

#if IS_ENABLED(CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT)
static bool brt_clamped = false;
static uint8_t saved_brt = 0;

static void clamp_brightness(void) {
    if (brt_clamped) {
        return;
    }
    struct zmk_led_hsb hsb = zmk_rgb_underglow_calc_brt(0);
    uint8_t cap = CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT;
    if (hsb.b > cap) {
        saved_brt = hsb.b;
        hsb.b = cap;
        zmk_rgb_underglow_set_hsb(hsb);
        brt_clamped = true;
    }
}

static void restore_brightness(void) {
    if (!brt_clamped) {
        return;
    }
    struct zmk_led_hsb hsb = zmk_rgb_underglow_calc_brt(0);
    hsb.b = saved_brt;
    zmk_rgb_underglow_set_hsb(hsb);
    brt_clamped = false;
}
#endif

static void update_state(void) {
    enum zmk_activity_state activity = zmk_activity_get_state();
    bool usb_powered = zmk_usb_is_powered();

    if (activity == ZMK_ACTIVITY_ACTIVE || usb_powered) {
        /* Active or on USB - restore if we auto-offed */
        if (auto_off_active) {
            gpio_pin_set_dt(&ext_power_gpio, 1);
            auto_off_active = false;
        }
#if IS_ENABLED(CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT)
        if (usb_powered) {
            restore_brightness();
        } else {
            clamp_brightness();
        }
#endif
    } else if (activity == ZMK_ACTIVITY_IDLE && !usb_powered) {
        /* On battery + idle - auto-off if user has ext-power on */
        if (!auto_off_active) {
            if (device_is_ready(ext_power_dev) && ext_power_get(ext_power_dev)) {
                gpio_pin_set_dt(&ext_power_gpio, 0);
                auto_off_active = true;
            }
        }
    }
}

static int ext_power_smart_idle_listener(const zmk_event_t *eh) {
    update_state();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ext_power_smart_idle, ext_power_smart_idle_listener);
ZMK_SUBSCRIPTION(ext_power_smart_idle, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(ext_power_smart_idle, zmk_usb_conn_state_changed);
