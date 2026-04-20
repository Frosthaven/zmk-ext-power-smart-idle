# zmk-ext-power-smart-idle

A ZMK module that adds USB-aware ext-power idle behavior. External power stays on while USB is connected (charging) and auto-offs after the idle timeout when on battery. Optionally clamps RGB brightness when on battery to reduce power draw.

This is particularly useful for boards with RGB LEDs on a switched power rail, but works with any ext-power controlled peripheral.

## Behavior

| Situation | Ext Power |
|---|---|
| Typing on battery | On (brightness clamped if configured) |
| Idle on battery (after timeout) | **Off** (auto, resumes on keypress) |
| Typing on USB | On (full brightness) |
| Idle on USB | **On** (stays on indefinitely, full brightness) |
| Plug in USB while auto-offed | Restored immediately (full brightness) |
| Unplug USB | Brightness clamped (if configured) |
| Unplug USB while idle | Off (auto) |
| Manual ext power toggle off | Off (module respects this and won't override) |
| Reboot after auto-off | Restores last manual toggle state |

## How It Works

The module listens for two ZMK events:
- **Activity state changes** (idle/active) - triggered by ZMK's idle timeout (default 30 seconds)
- **USB connection changes** (plugged/unplugged)

When both events indicate "idle + no USB," the module toggles the ext-power GPIO pin (defined in your devicetree overlay) to cut external power. When either condition changes (keypress or USB plugged in), it restores power.

### No Flash Writes

The module toggles the ext-power GPIO pin directly instead of using ZMK's ext_power API. Brightness clamping uses `zmk_rgb_underglow_set_hsb()` which only modifies runtime state. This means **zero flash writes** during auto idle/wake cycles and brightness changes, preserving flash endurance. Only manual toggles (like `RGB_TOG` with `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER=y`, or `&ext_power EP_TOG`) save state to flash via ZMK's built-in behavior.

### Respects Manual Toggle

The module checks the ext-power saved state before acting. If you manually turn off ext-power (via `RGB_TOG` with `CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER=y`, or `&ext_power EP_TOG`), the module sees that the user wants power off and will not auto-restore it. This works regardless of whether RGB underglow is enabled.

### Per-Half Independent (Split Keyboards)

On split keyboards, each half checks its own USB state independently. If only one half is plugged in, that half keeps external power on while the other half follows the idle timeout.

## Requirements

- External power control configured (`CONFIG_ZMK_EXT_POWER=y` with an `ext-power` devicetree node)
- USB device stack (`CONFIG_USB_DEVICE_STACK=y` - enabled by default on nice!nano)
- RGB underglow (`CONFIG_ZMK_RGB_UNDERGLOW=y`) is optional - the module works with any ext-power controlled peripheral

## Installation

### 1. Add to `config/west.yml`

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

### 2. Update your `.conf` file

```ini
# Disable built-in auto-off if using RGB (this module replaces it)
CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE=n

# Enable smart idle
CONFIG_ZMK_EXT_POWER_SMART_IDLE=y

# Optional: clamp RGB brightness to 20% when on battery (requires CONFIG_ZMK_RGB_UNDERGLOW=y)
CONFIG_ZMK_EXT_POWER_SMART_IDLE_BATTERY_BRT=20
```

### Idle Timeout

The module uses ZMK's built-in idle timeout to determine when the keyboard is inactive. By default this is 30 seconds. You can change it in your `.conf` file:

```ini
CONFIG_ZMK_IDLE_TIMEOUT=60000
```

The value is in milliseconds. Note that this setting also affects when ZMK starts the deep sleep countdown, so changing it applies system-wide, not just to this module.

### 3. Ensure ext-power is configured

Your board overlay needs an `ext-power` node:

```dts
/ {
    ext-power {
        compatible = "zmk,ext-power-generic";
        control-gpios = <&gpio0 10 GPIO_ACTIVE_LOW>;
    };
};
```

## License

MIT
