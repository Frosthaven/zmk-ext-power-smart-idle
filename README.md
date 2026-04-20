# zmk-ext-power-smart-idle

A ZMK module that makes your keyboard's LEDs smarter about battery life. It automatically manages LED power based on whether you're plugged in, typing, or idle — so you get the best experience without manually toggling things.

## What It Does

**When plugged in via USB**, your LEDs stay on at full brightness all the time. No need to worry about battery.

**When on battery**, the module does three things to save power:

1. **Dims your LEDs** to a lower brightness (default 20%) so they use less energy
2. **Turns LEDs off when you stop typing** (after the idle timeout, default 30 seconds) and turns them back on when you start again
3. **Turns LEDs off at low battery** (optional, e.g. at 5%) to save the remaining power for typing

**When you plug USB back in**, full brightness is restored automatically.

The module also respects your manual toggles. If you turn LEDs off yourself, it won't fight you and turn them back on.

## At a Glance

| Situation | LEDs |
|---|---|
| Typing, plugged in | On, full brightness |
| Idle, plugged in | On, full brightness |
| Typing, on battery | On, dimmed (e.g. 20%) |
| Idle, on battery | Off (auto-restores on keypress) |
| Low battery (if configured) | Off (auto-restores when plugged in) |
| You manually turned LEDs off | Off (module won't override you) |

## Setup

### Step 1: Add the module to your `config/west.yml`

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

### Step 2: Update your `.conf` file

Add these lines to your shield's `.conf` file:

```ini
# Disable ZMK's built-in auto-off (this module replaces it)
CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE=n

# Enable smart idle
CONFIG_ZMK_EXT_POWER_SMART_IDLE=y

# Dim LEDs to 20% when on battery (requires CONFIG_ZMK_RGB_UNDERGLOW=y)
CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT=20

# Turn LEDs off when battery drops to 5% (requires CONFIG_ZMK_BATTERY=y)
# Set to 0 to disable
CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF=5
```

### Step 3: Make sure ext-power is configured

Your board overlay needs an `ext-power` node. Most boards with RGB already have this. If not, add it:

```dts
/ {
    ext-power {
        compatible = "zmk,ext-power-generic";
        control-gpios = <&gpio0 10 GPIO_ACTIVE_LOW>;
    };
};
```

### Changing the Idle Timeout

By default, ZMK considers you "idle" after 30 seconds of no keypresses. You can change this:

```ini
CONFIG_ZMK_IDLE_TIMEOUT=60000
```

The value is in milliseconds (60000 = 1 minute). This is a system-wide ZMK setting that also affects deep sleep countdown.

## Configuration Reference

| Setting | Default | Description |
|---|---|---|
| `CONFIG_ZMK_EXT_POWER_SMART_IDLE` | `n` | Enable the module |
| `CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT` | `20` | Max LED brightness (0-100) when on battery. Requires `CONFIG_ZMK_RGB_UNDERGLOW=y` |
| `CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_CUTOFF` | `0` | Battery % at which LEDs turn off completely. 0 = disabled. Requires `CONFIG_ZMK_BATTERY=y` |

## Requirements

- `CONFIG_ZMK_EXT_POWER=y` with an `ext-power` devicetree node
- `CONFIG_USB_DEVICE_STACK=y` (enabled by default on nice!nano)
- `CONFIG_ZMK_RGB_UNDERGLOW=y` needed for brightness clamping (optional otherwise)
- `CONFIG_ZMK_BATTERY=y` needed for low battery cutoff (optional otherwise)

## Battery Life Estimates

Estimates below assume a split keyboard with 27 WS2812-compatible LEDs per half (6 underglow + 21 per-key), ~15mA base board draw, and brightness clamped to 20% on battery. All figures are per half.

### Wireless (per half, with 5% battery cutoff)

With a 5% battery cutoff, LEDs run until the battery drops to 5%, then turn off to preserve typing time.

| Scenario | 750mAh (LEDs on) | 750mAh (after cutoff) | 1500mAh (LEDs on) | 1500mAh (after cutoff) |
|---|---|---|---|---|
| 20% single color, always on | 5.8 hrs | 2.5 hrs | 11.6 hrs | 5 hrs |
| Mixed use, 50% typing | 8.6 hrs | 2.5 hrs | 17.2 hrs | 5 hrs |
| Mixed use, 30% typing | 10.8 hrs | 2.5 hrs | 21.6 hrs | 5 hrs |
| LEDs off (idle/sleep) | — | 17.9 hrs | — | 35.7 hrs |

The "after cutoff" column is typing time remaining at ~15mA board-only draw once LEDs shut off.

### Charging

The nice!nano v2 charges at 100mA by default. Soldering the charge rate jumper on the nice!nano v2 increases the charge rate to 500mA.

| Battery | @ 100mA | @ 500mA |
|---|---|---|
| 750mAh | ~7.5 hrs | ~1.5 hrs |
| 1500mAh (2x parallel) | ~15 hrs | ~3 hrs |

At 100mA, the charger is slower than the LED draw at full brightness, so the battery will slowly drain even while plugged in. At 500mA, the board charges even with LEDs on at full brightness.

| LED State (27 LEDs) | Draw | Net @ 100mA | Net @ 500mA |
|---|---|---|---|
| 50% single color | 285mA | -185mA (drain) | +215mA (charge) |
| 20% single color | 123mA | -23mA (drain) | +377mA (charge) |
| LEDs off | 42mA | +58mA (charge) | +458mA (charge) |

## Technical Details

### How It Works

The module listens for three ZMK events:
- **Activity state changes** (idle/active) - triggered by ZMK's idle timeout
- **USB connection changes** (plugged/unplugged)
- **Battery level changes** (if cutoff is enabled)

When conditions change, it evaluates the combined state and decides whether to turn LEDs on/off or adjust brightness.

### No Flash Writes

The module toggles the ext-power GPIO pin directly instead of using ZMK's ext_power API. Brightness clamping uses `zmk_rgb_underglow_set_hsb()` which only modifies runtime state. This means **zero flash writes** during automatic cycles, preserving your microcontroller's flash endurance. Only your manual toggles (like `RGB_TOG` or `EP_TOG` keypresses) write to flash through ZMK's built-in behavior.

### Respects Manual Toggle

If you manually turn off ext-power (via `RGB_TOG` with `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER=y`, or `&ext_power EP_TOG`), the module sees that you want power off and will not override your choice.

### Split Keyboard Support

On split keyboards, each half checks its own USB state independently. If only one half is plugged in, that half keeps LEDs on while the other half follows the battery-saving rules.

## License

MIT
