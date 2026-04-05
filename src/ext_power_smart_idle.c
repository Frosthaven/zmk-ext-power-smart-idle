/*
 * Copyright (c) 2026 Frosthaven
 * SPDX-License-Identifier: MIT
 *
 * Smart ext-power idle module for ZMK.
 * Skips ext-power idle timeout when USB is connected (charging).
 * On battery, auto-offs ext-power after ZMK idle timeout and restores on keypress.
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

#define EXT_POWER_NODE DT_NODELABEL(ext_power)

static const struct gpio_dt_spec ext_power_gpio =
    GPIO_DT_SPEC_GET(EXT_POWER_NODE, control_gpios);

static const struct device *ext_power_dev = DEVICE_DT_GET(EXT_POWER_NODE);

static bool auto_off_active = false;

static void update_state(void) {
    enum zmk_activity_state activity = zmk_activity_get_state();
    bool usb_powered = zmk_usb_is_powered();

    if (activity == ZMK_ACTIVITY_ACTIVE || usb_powered) {
        /* Active or on USB - restore if we auto-offed */
        if (auto_off_active) {
            gpio_pin_set_dt(&ext_power_gpio, 1);
            auto_off_active = false;
        }
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
