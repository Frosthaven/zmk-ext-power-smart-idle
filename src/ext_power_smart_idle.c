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

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_SMART_IDLE_SYNC) &&                                            \
    IS_ENABLED(CONFIG_ZMK_EXT_POWER_SMART_IDLE_SYNC_HALVES)
#include <zmk/events/split_remote_smart_idle_state_changed.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#else
#include <zmk/split/bluetooth/service.h>
#endif
#define SMART_IDLE_SYNC_ENABLED 1
#else
#define SMART_IDLE_SYNC_ENABLED 0
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_RUNTIME_CAP) &&                                            \
    IS_ENABLED(CONFIG_ZMK_EXT_POWER_SMART_IDLE_ENFORCE_BRT_CAP)
/* The runtime cap is set via zmk_rgb_underglow_set_runtime_max_brightness()
 * (declared in zmk/rgb_underglow.h, already included above). The cap
 * is applied at the render layer so state.color.b is never mutated by
 * smart-idle - the user's pressed-up brightness accumulates normally
 * and persists to NVS at its intended value. */
#define SMART_IDLE_BRT_CAP_ENFORCE 1
/* "No cap" value passed to the setter when on USB. BRT_MAX is 100 in
 * ZMK and state.color.b can never exceed it, so 100 == disable cap. */
#define SMART_IDLE_RUNTIME_CAP_DISABLED 100
#else
#define SMART_IDLE_BRT_CAP_ENFORCE 0
#endif

/* On split peripheral builds CONFIG_ZMK_USB is off (the peripheral isn't
 * the USB HID device), so zmk_usb_is_powered() always returns false even
 * when the peripheral is plugged in for charging. Fall through to a
 * direct VBUS read via nrfx_power, the same trick battery.c uses for
 * ZMK_BATTERY_ENCODE_PERIPHERAL_CHARGING. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                  \
    IS_ENABLED(CONFIG_NRFX_POWER)
#define SMART_IDLE_PERIPHERAL_VBUS_DETECT 1
#include <hal/nrf_power.h>
#include <nrfx_power.h>
#endif

static inline bool smart_idle_usb_powered(void) {
#if defined(SMART_IDLE_PERIPHERAL_VBUS_DETECT)
    return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
#elif IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

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
    zmk_rgb_underglow_set_hsb_silent(hsb);
    fade_active = false;
}

static void fade_step_handler(struct k_work *work) {
    int64_t elapsed = k_uptime_get() - fade_start_time;
    struct zmk_led_hsb hsb = zmk_rgb_underglow_calc_brt(0);

    if (elapsed >= CONFIG_ZMK_EXT_POWER_SMART_IDLE_FADE_MS) {
        /* Fade complete - cut MOSFET. fade_active stays true so a wake
         * restores brightness; cleared in cancel_fade_and_restore. */
        hsb.b = 0;
        zmk_rgb_underglow_set_hsb_silent(hsb);
        gpio_pin_set_dt(&ext_power_gpio, 0);
        auto_off_active = true;
        /* Restore the in-RAM brightness back to the pre-fade value so any
         * still-debouncing NVS save scheduled by an earlier user RGB_BRI/
         * set_hsb (which serializes the shared rgb_underglow state struct
         * at debounce-fire time) captures the user's intended brightness
         * instead of the 0 we just walked it down to. The MOSFET is now
         * cut so the LEDs are dark regardless of state.color.b, making
         * the restore visually invisible. */
        hsb.b = fade_start_brt;
        zmk_rgb_underglow_set_hsb_silent(hsb);
        return;
    }

    uint32_t remaining = (uint32_t)(CONFIG_ZMK_EXT_POWER_SMART_IDLE_FADE_MS - elapsed);
    hsb.b = (uint8_t)((uint32_t)fade_start_brt * remaining /
                      CONFIG_ZMK_EXT_POWER_SMART_IDLE_FADE_MS);
    zmk_rgb_underglow_set_hsb_silent(hsb);
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
    if (!smart_idle_usb_powered() || zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE) {
        return;
    }
    start_fade_off();
}
static K_WORK_DELAYABLE_DEFINE(usb_idle_off_work, usb_idle_off_handler);
#endif

#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT > 0

#if SMART_IDLE_BRT_CAP_ENFORCE
/* Render-layer cap path (preferred). state.color.b is never mutated
 * by clamp/restore; we just slide the soft cap that hsb_scale_min_max
 * applies. NVS preserves the user's pressed-up brightness exactly. */
static void clamp_brightness(void) {
    zmk_rgb_underglow_set_runtime_max_brightness(
        CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT);
}

static void restore_brightness(void) {
    zmk_rgb_underglow_set_runtime_max_brightness(SMART_IDLE_RUNTIME_CAP_DISABLED);
}
#else
/* Legacy one-shot mutation path for stock-ZMK builds without the
 * runtime cap feature. Cap fires once at the USB->battery transition
 * and pins state.color.b at the cap value; the next RGB_BRI press can
 * lift it back above the cap (known limitation - upgrade the fork +
 * enable RUNTIME_CAP + ENFORCE_BRT_CAP to fix). */
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
        zmk_rgb_underglow_set_hsb_silent(hsb);
        brt_clamped = true;
    }
}

static void restore_brightness(void) {
    if (!brt_clamped) {
        return;
    }
    struct zmk_led_hsb hsb = zmk_rgb_underglow_calc_brt(0);
    hsb.b = saved_brt;
    zmk_rgb_underglow_set_hsb_silent(hsb);
    brt_clamped = false;
}
#endif /* SMART_IDLE_BRT_CAP_ENFORCE */

#endif /* CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT > 0 */

static void update_state(void);

#if SMART_IDLE_SYNC_ENABLED

/* Tracks what the other half last broadcast so update_state can apply
 * the combined policy: ACTIVE if either half is active; battery cutoff
 * if either half is below cutoff. */
static bool remote_active = false;
static bool remote_battery_below_cutoff = false;
static uint8_t last_sent_state = 0xFF;

static uint8_t local_state_byte(void) {
    uint8_t s = 0;
    if (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE) {
        s |= 0x01;
    }
#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF > 0
    /* Mirror the same threshold check update_state uses so peers learn
     * immediately rather than waiting for the local fade to start. */
    if (!smart_idle_usb_powered()) {
        uint8_t lvl = zmk_battery_state_of_charge();
        if (lvl > 0 && lvl <= CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF) {
            s |= 0x02;
        }
    }
#endif
    return s;
}

static void send_local_state(void) {
    uint8_t s = local_state_byte();
    if (s == last_sent_state) {
        return;
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    int ret = zmk_split_central_set_central_smart_idle_state(s);
#else
    int ret = zmk_split_bt_service_notify_smart_idle_state(s);
#endif
    if (ret >= 0) {
        last_sent_state = s;
    }
}

static int smart_idle_remote_state_listener(const zmk_event_t *eh) {
    const struct zmk_split_remote_smart_idle_state_changed *ev =
        as_zmk_split_remote_smart_idle_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    remote_active = ev->active;
    remote_battery_below_cutoff = ev->battery_below_cutoff;
    /* TEMP DEBUG (remove after diagnosing trackpad-build central->peripheral
     * wake): if a "remote half is ACTIVE" message reaches this listener, the
     * central->peripheral write IS being delivered. Force the LED rail on
     * unconditionally here, bypassing update_state()'s combined-state logic,
     * so we can see it with no console:
     *   - peripheral lights up when you interact with the central  => the
     *     write arrives; the bug is downstream in the wake logic (and this
     *     line is effectively the fix).
     *   - peripheral stays dark                                    => the
     *     write never arrives; it's a delivery problem. */
    if (ev->active) {
        cancel_fade_and_restore();
        if (device_is_ready(ext_power_dev)) {
            gpio_pin_set_dt(&ext_power_gpio, 1);
        }
        auto_off_active = false;
    }
    update_state();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ext_power_smart_idle_remote, smart_idle_remote_state_listener);
ZMK_SUBSCRIPTION(ext_power_smart_idle_remote, zmk_split_remote_smart_idle_state_changed);

#else /* !SMART_IDLE_SYNC_ENABLED */

#define remote_active false
#define remote_battery_below_cutoff false
static inline void send_local_state(void) {}

#endif /* SMART_IDLE_SYNC_ENABLED */

/* (The previous per-event brt_cap_listener has been removed. With
 * the render-layer runtime cap, the cap is applied during pixel
 * rendering inside hsb_scale_min_max, so we don't need to react to
 * every user brightness key press - state.color.b is allowed to climb
 * to BRT_MAX and persist, and the visual stays under BATTERY_BRT
 * while the cap is set. The cap is toggled in update_state() below
 * on USB plug / unplug transitions via clamp_brightness() and
 * restore_brightness(), which now call
 * zmk_rgb_underglow_set_runtime_max_brightness() instead of mutating
 * state.color.b.) */

static void update_state(void) {
    enum zmk_activity_state activity = zmk_activity_get_state();
    bool usb_powered = smart_idle_usb_powered();

    /* Combined state used by the idle/cutoff branches. When SYNC_HALVES
     * is off, remote_* are compile-time false and effective_* collapse
     * to the local-only behavior. */
    bool effective_active = (activity == ZMK_ACTIVITY_ACTIVE) || remote_active;

#if CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF > 0
    uint8_t battery_level = zmk_battery_state_of_charge();
    uint8_t cutoff = CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF;
    bool local_below_cutoff =
        (!usb_powered && battery_level <= cutoff && battery_level > 0);
    bool effective_below_cutoff = local_below_cutoff || remote_battery_below_cutoff;

    if (effective_below_cutoff && !usb_powered) {
        /* Either half is below cutoff while wireless - fade out and cut.
         * The fade uses the same FADE_MS setting as the idle paths so the
         * user gets a consistent visual cue for any auto-off event. */
        if (!battery_cutoff_active) {
            battery_cutoff_active = true;
            start_fade_off();
        }
        send_local_state();
        return;
    }

    if (battery_cutoff_active && (usb_powered || !effective_below_cutoff)) {
        battery_cutoff_active = false;
        /* Fall through to normal logic to restore state */
    }
#endif

    if (effective_active || usb_powered) {
        if (activity == ZMK_ACTIVITY_ACTIVE) {
            active_state_seen = true;
        }
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
        } else if (usb_powered && activity == ZMK_ACTIVITY_IDLE && !remote_active) {
            /* USB plugged + locally idle (and remote not active) - start
             * countdown to auto-off. k_work_schedule is a no-op if already
             * scheduled, so repeated events while idle don't reset it. */
            k_work_schedule(&usb_idle_off_work,
                            K_SECONDS(CONFIG_ZMK_EXT_POWER_SMART_IDLE_USB_TIMEOUT_S));
        } else if (remote_active) {
            /* Remote half just went active - cancel pending USB-idle off */
            k_work_cancel_delayable(&usb_idle_off_work);
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
        /* On battery + both halves idle - auto-off if user has ext-power on.
         * Skipped until we've observed at least one ACTIVE state so a
         * boot-time IDLE event doesn't shut us off. With SYNC_HALVES on,
         * remote_active is also factored into effective_active above; we
         * only reach this branch when both halves are idle. */
        if (!auto_off_active) {
            start_fade_off();
        }
    }

    send_local_state();
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
