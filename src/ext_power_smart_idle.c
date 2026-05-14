/*
 * Copyright (c) 2026 Frosthaven
 * SPDX-License-Identifier: MIT
 *
 * Smart ext-power idle module for ZMK.
 * Skips ext-power idle timeout when USB is connected (charging).
 * On battery, auto-offs ext-power after ZMK idle timeout and restores on keypress.
 * Optionally clamps RGB brightness when on battery to reduce power draw.
 * Optionally cuts LEDs entirely at a low battery threshold.
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

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF > 0
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#define EXT_POWER_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_ext_power_generic)

#if !DT_NODE_HAS_STATUS(EXT_POWER_NODE, okay)
#error "no enabled zmk,ext-power-generic node"
#endif

static const struct gpio_dt_spec ext_power_gpio =
    GPIO_DT_SPEC_GET(EXT_POWER_NODE, control_gpios);

static const struct device *ext_power_dev = DEVICE_DT_GET(EXT_POWER_NODE);

static bool auto_off_active = false;

/* Prevent auto-off during the brief window after boot, before USB has been
 * detected as powered. Without this, the listener can fire on the very first
 * activity_state_changed event (with usb_powered=false) and shut the MOSFET
 * off the moment we just enabled it in sync_at_boot. */
static bool active_state_seen = false;

#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF > 0
static bool battery_cutoff_active = false;
#endif

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

#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF > 0
    uint8_t battery_level = zmk_battery_state_of_charge();
    uint8_t cutoff = CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF;

    if (!usb_powered && battery_level <= cutoff && battery_level > 0) {
        /* Battery below cutoff while wireless - force LEDs off */
        if (!battery_cutoff_active) {
            if (device_is_ready(ext_power_dev) && ext_power_get(ext_power_dev)) {
                gpio_pin_set_dt(&ext_power_gpio, 0);
                battery_cutoff_active = true;
                auto_off_active = true;
            }
        }
        return;
    }

    if (battery_cutoff_active && (usb_powered || battery_level > cutoff)) {
        battery_cutoff_active = false;
        /* Fall through to normal logic to restore state */
    }
#endif

    if (activity == ZMK_ACTIVITY_ACTIVE || usb_powered) {
        active_state_seen = true;
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
    } else if (activity == ZMK_ACTIVITY_IDLE && !usb_powered && active_state_seen) {
        /* On battery + idle - auto-off if user has ext-power on. Skipped until
         * we've observed at least one ACTIVE state, so a boot-time IDLE event
         * (fired before USB enumeration completes) doesn't shut us off. */
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
#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF > 0
ZMK_SUBSCRIPTION(ext_power_smart_idle, zmk_battery_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
/* ZMK's RGB_UNDERGLOW_ON_START sets internal state to "on" and starts the LED
 * tick, but does not call ext_power_enable() at boot. Mirror RGB on/off into
 * ext-power at startup so the rail actually has voltage when the firmware
 * thinks RGB is on. */
static int ext_power_smart_idle_sync_at_boot(void) {
    bool rgb_on = false;
    if (zmk_rgb_underglow_get_state(&rgb_on) != 0) {
        return 0;
    }
    if (!device_is_ready(ext_power_dev)) {
        return 0;
    }
    int ext_on = ext_power_get(ext_power_dev);
    if (rgb_on && ext_on == 0) {
        ext_power_enable(ext_power_dev);
    } else if (!rgb_on && ext_on > 0) {
        ext_power_disable(ext_power_dev);
    }
    return 0;
}
SYS_INIT(ext_power_smart_idle_sync_at_boot, APPLICATION, 91);
#endif
