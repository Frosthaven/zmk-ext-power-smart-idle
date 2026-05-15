# zmk-ext-power-smart-idle

A ZMK module that automatically manages your keyboard's external power rail and RGB brightness for longer battery life. It reacts to USB connection, typing, idle, and low battery, so you get the best experience without manually toggling things.

## What It Does

**When plugged in via USB**, your LEDs stay on at full brightness. Optionally, you can configure an inactivity timeout so the LEDs auto-off after a long stretch of idle (e.g. 2 hours), and switch back on as soon as you type again.

**When on battery**, the module does three things to save power:

1. **Caps your LED brightness** at a lower value (default 20%) so they use less energy. Your saved brightness preference is preserved and restored when USB is reconnected
2. **Turns LEDs off when you stop typing** (after the ZMK idle timeout, default 30 seconds) and turns them back on when you start again
3. **Turns LEDs off at low battery** (optional, e.g. at 5%) to save the remaining power for typing

**When you plug USB back in**, full brightness is restored automatically.

The module also respects your manual toggles. If you turn LEDs off yourself, it won't fight you and turn them back on.

## At a Glance

| Situation | LEDs |
|---|---|
| Typing, plugged in | On, full brightness |
| Idle, plugged in | On, full brightness |
| Idle, plugged in, past USB timeout (if configured) | Off (auto-restores on keypress) |
| Typing, on battery | On, capped (e.g. 20%) |
| Idle, on battery | Off (auto-restores on keypress) |
| Low battery (if configured) | Off (auto-restores when plugged in) |
| You manually turned LEDs off | Off (module won't override you) |

## Setup

### Step 1: Wire your LED rail through a MOSFET

For auto-off to actually cut power, the LED chain's VCC must go through a P-channel MOSFET that the ZMK GPIO can switch off. Wire it like this:

```
VCC                -> MOSFET source
ZMK EXT_POWER GPIO -> MOSFET gate
MOSFET drain       -> LED_POWER
```

Any P-channel SOT-23 MOSFET with similar specs to [AOS AO3401A (LCSC C15127)](https://www.lcsc.com/product-detail/MOSFET_Alpha-Omega-Semicon-AOS_AO3401A_C15127.html) works.

If you wire LED_POWER directly to a power rail with no MOSFET, the brightness clamp still works (it just changes the color data), but the idle / battery-cutoff / USB-timeout auto-offs cannot cut power and are effectively no-ops.

### Step 2: Add the module to your `config/west.yml`

Add the `frosthaven` remote and the `zmk-ext-power-smart-idle` project to your manifest. Here's a minimal example:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: frosthaven
      url-base: https://github.com/Frosthaven
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-ext-power-smart-idle
      remote: frosthaven
      revision: main
  self:
    path: config
```

### Step 3: Make sure ext-power is configured

Your board overlay needs an `ext-power` node pointing at the GPIO that drives the MOSFET gate from Step 1. Most boards with RGB already have this. If not, add it:

```dts
/ {
    ext-power {
        compatible = "zmk,ext-power-generic";
        control-gpios = <&gpio0 10 GPIO_ACTIVE_LOW>;
    };
};
```

### Step 4: Update your `.conf` file

Add these lines to your shield's `.conf` file:

```ini
# Disable ZMK's built-in auto-off (this module replaces it)
CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE=n

# Enable smart idle
CONFIG_ZMK_EXT_POWER_SMART_IDLE=y

# Cap LED brightness at 20% when on battery (requires CONFIG_ZMK_RGB_UNDERGLOW=y)
# Set to 0 to disable brightness clamping
CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT=20

# Turn LEDs off when battery drops to 5% (requires CONFIG_ZMK_BATTERY=y)
# Set to 0 to disable
CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF=5

# Optional: auto-off ext-power after 2 hours of idle even on USB
# Set to 0 to keep LEDs on indefinitely while plugged in
CONFIG_ZMK_EXT_POWER_SMART_IDLE_USB_TIMEOUT_S=7200
```

### Changing the Idle Timeout

By default, ZMK considers you "idle" after 30 seconds of no keypresses. You can change this:

```ini
CONFIG_ZMK_IDLE_TIMEOUT=60000
```

The value is in milliseconds (60000 = 1 minute). This is a system-wide ZMK setting that also affects deep sleep countdown.

ZMK fires its IDLE event after `ZMK_IDLE_TIMEOUT`. On battery the module auto-offs immediately at that point. On USB it then starts the separate `USB_TIMEOUT_S` countdown, so the total wait on USB is `ZMK_IDLE_TIMEOUT + USB_TIMEOUT_S` (defaults: 30s + 7200s ≈ 2 hours).

## Configuration Reference

| Setting | Default | Description |
|---|---|---|
| `CONFIG_ZMK_EXT_POWER_SMART_IDLE` | `n` | Enable the module |
| `CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT` | `20` | Max LED brightness (0-100) when on battery. 0 disables the clamp. Requires `CONFIG_ZMK_RGB_UNDERGLOW=y` |
| `CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF` | `0` | Battery % at which LEDs turn off completely. 0 disables. Requires `CONFIG_ZMK_BATTERY=y` |
| `CONFIG_ZMK_EXT_POWER_SMART_IDLE_USB_TIMEOUT_S` | `0` | Seconds of idle on USB before ext-power auto-offs. 0 keeps LEDs on indefinitely while plugged in. Restored on the next ACTIVE state |

## Requirements

- `CONFIG_ZMK_EXT_POWER=y` with an `ext-power` devicetree node
- `CONFIG_USB_DEVICE_STACK=y` (enabled by default on nice!nano)
- `CONFIG_ZMK_RGB_UNDERGLOW=y` needed for brightness clamping (optional otherwise)
- `CONFIG_ZMK_BATTERY=y` needed for low battery cutoff (optional otherwise)

## Technical Details

### How It Works

The module listens for three ZMK events:
- **Activity state changes** (idle/active), triggered by ZMK's idle timeout
- **USB connection changes** (plugged/unplugged)
- **Battery level changes** (if cutoff is enabled)

When conditions change, it evaluates the combined state and decides whether to turn LEDs on/off or adjust brightness. The optional USB inactivity timeout uses a delayable work item that gets scheduled when activity transitions to idle on USB and cancelled when it returns to active.

### No Flash Writes

The module toggles the ext-power GPIO pin directly instead of using ZMK's ext_power API. Brightness clamping uses `zmk_rgb_underglow_set_hsb()` which only modifies runtime state. This means **zero flash writes** during automatic cycles, preserving your microcontroller's flash endurance. Only your manual toggles (like `RGB_TOG` or `EP_TOG` keypresses) write to flash through ZMK's built-in behavior.

### Respects Manual Toggle

If you manually turn off ext-power (via `RGB_TOG` with `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER=y`, or `&ext_power EP_TOG`), the module sees that you want power off and will not override your choice.

### Split Keyboard Support

On split keyboards, each half checks its own USB state independently. If only one half is plugged in, that half keeps LEDs on while the other half follows the battery-saving rules.

## License

MIT
</content>
</invoke>