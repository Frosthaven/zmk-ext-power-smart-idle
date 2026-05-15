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

/* Idle-driven ext-power off path. When fade is enabled, linearly fade RGB
 * brightness from current to 0 over FADE_MS, then cut the MOSFET. When fade
 * is disabled (or RGB underglow is not present), cut the MOSFET immediately.
 * In both cases cancel_fade_and_restore() restores pre-fade brightness on
 * wake without touching the persisted (NVS) brightness setting. */
#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_FADE_MS > 0

#define FADE_STEP_MS 50

static bool fade_active = false;
static uint8_t fade_start_brt = 0;
static int64_t fade_start_time = 0;

static void fade_step_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(fade_work, fade_step_handler);

static void start_fade_off(void) {
    if (fade_active) {
        return;
    }
    if (!device_is_ready(ext_power_dev) || !ext_power_get(ext_power_dev)) {
        return;
    }
    struct zmk_led_hsb hsb = zmk_rgb_underglow_calc_brt(0);
    if (hsb.b == 0) {
        /* Nothing to fade - cut the MOSFET immediately. */
        gpio_pin_set_dt(&ext_power_gpio, 0);
        auto_off_active = true;
        return;
    }
    fade_start_brt = hsb.b;
    fade_start_time = k_uptime_get();
    fade_active = true;
    k_work_schedule(&fade_work, K_NO_WAIT);
}

static void cancel_fade_and_restore(void) {
    if (!fade_active) {
        return;
    }
    k_work_cancel_delayable(&fade_work);
    struct zmk_led_hsb hsb = zmk_rgb_underglow_calc_brt(0);
    hsb.b = fade_start_brt;
    zmk_rgb_underglow_set_hsb(hsb);
    fade_active = false;
}

static void fade_step_handler(struct k_work *work) {
    int64_t elapsed = k_uptime_get() - fade_start_time;
    struct zmk_led_hsb hsb = zmk_rgb_underglow_calc_brt(0);

    if (elapsed >= CONFIG_ZMK_EXT_POWER_SMART_IDLE_FADE_MS) {
        /* Fade complete - cut MOSFET. fade_active stays true so a wake
         * restores brightness; cleared in cancel_fade_and_restore. */
        hsb.b = 0;
        zmk_rgb_underglow_set_hsb(hsb);
        gpio_pin_set_dt(&ext_power_gpio, 0);
        auto_off_active = true;
        return;
    }

    uint32_t remaining = (uint32_t)(CONFIG_ZMK_EXT_POWER_SMART_IDLE_FADE_MS - elapsed);
    hsb.b = (uint8_t)((uint32_t)fade_start_brt * remaining /
                      CONFIG_ZMK_EXT_POWER_SMART_IDLE_FADE_MS);
    zmk_rgb_underglow_set_hsb(hsb);
    k_work_schedule(&fade_work, K_MSEC(FADE_STEP_MS));
}

#else /* fade disabled - keep the original instant-off path. */

static inline void start_fade_off(void) {
    if (device_is_ready(ext_power_dev) && ext_power_get(ext_power_dev)) {
        gpio_pin_set_dt(&ext_power_gpio, 0);
        auto_off_active = true;
    }
}
static inline void cancel_fade_and_restore(void) {}

#endif /* CONFIG_ZMK_EXT_POWER_SMART_IDLE_FADE_MS > 0 */

#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_USB_TIMEOUT_S > 0
static void usb_idle_off_handler(struct k_work *work) {
    /* Re-check state when timer fires - user may have come back or unplugged */
    if (!zmk_usb_is_powered() || zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE) {
        return;
    }
    start_fade_off();
}
static K_WORK_DELAYABLE_DEFINE(usb_idle_off_work, usb_idle_off_handler);
#endif

#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT > 0
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
        /* Battery below cutoff while wireless - fade out, then cut MOSFET.
         * The fade uses the same FADE_MS setting as the idle paths so the
         * user gets a consistent visual cue for any auto-off event. */
        if (!battery_cutoff_active) {
            battery_cutoff_active = true;
            start_fade_off();
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
        /* Active or on USB - cancel any in-progress fade and restore the
         * pre-fade brightness in runtime state before re-enabling MOSFET so
         * the LEDs come back up at the brightness the user expects. */
        cancel_fade_and_restore();
        if (auto_off_active) {
            gpio_pin_set_dt(&ext_power_gpio, 1);
            auto_off_active = false;
        }
#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_USB_TIMEOUT_S > 0
        if (activity == ZMK_ACTIVITY_ACTIVE) {
            /* User active - cancel any pending USB-idle countdown */
            k_work_cancel_delayable(&usb_idle_off_work);
        } else if (usb_powered && activity == ZMK_ACTIVITY_IDLE) {
            /* USB plugged + idle - start countdown to auto-off.
             * k_work_schedule is a no-op if already scheduled, so repeated
             * events while idle don't reset the timer. */
            k_work_schedule(&usb_idle_off_work,
                            K_SECONDS(CONFIG_ZMK_EXT_POWER_SMART_IDLE_USB_TIMEOUT_S));
        }
#endif
#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT > 0
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
            start_fade_off();
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
