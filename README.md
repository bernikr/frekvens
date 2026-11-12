# Frekvens

ESPHome external component for the **IKEA Frekvens LED panel** (16×16, 256 white
LEDs). The panel is driven through its original SCT2024 shift-register chain and
uses **Binary Code Modulation (BAM)** for grayscale, so it works as a normal
ESPHome `display` with the drawing API (`it.print`, `it.image`, `it.line`, …).

This replaces the old `ikea_frekvens` / `ikea_frekvens_simple` experiments in this
repository.

## How it works

The panel contains 16 SCT2024 16-bit constant-current LED drivers in series.
They only switch LEDs on or off, so grayscale is produced by splitting every
frame into seven bitplanes:

| Plane | Display time |
| :--- | :--- |
| 0 | 100 µs |
| 1 | 200 µs |
| 2 | 400 µs |
| 3 | 800 µs |
| 4 | 1.6 ms |
| 5 | 3.2 ms |
| 6 | 6.4 ms |

One frame is `254 × 50 µs = 12.7 ms`, i.e. **~78 Hz**, with **128 gray levels**
(7-bit BAM). The LSB of an 8-bit value would be a 50 µs slot; the SPI transfer
plus task wake-up currently takes ~50 µs, so that slot cannot be met reliably.
Dropping only the least significant bit avoids timing misses and keeps the frame
rate high. The result is visually smooth; 128 levels are plenty for a 16×16
panel.

A hardware timer (`gptimer`) latches each bitplane. A dedicated high-priority
FreeRTOS task shifts the next bitplane into the chain while the current one is
visible. If a transfer is not finished when the next latch is due, the latch is
deferred by one tick instead of displaying a partially shifted frame. The
`late latch` counter in the debug log tracks these events.

The task and timer are **suspended while OTA writes flash**. This is important:
the task runs at a higher priority than the ESPHome loop and could otherwise
preempt a cache-off flash operation, which trips the task watchdog. The
`ota: on_begin` automation in `frekvens.yaml` calls `suspend()`; `on_abort` and
`on_error` call `resume()`.

## Hardware / pinout

| Signal | GPIO | Notes |
| :--- | :--- | :--- |
| Data (MOSI) | 27 | SCT2024 serial data |
| Clock | 14 | SCT2024 clock (SPI, mode 0, LSB-first) |
| Latch (LA) | 19 | SCT2024 latch, active/transparent high |
| Enable (OE) | 26 | Active low; optional but recommended |

The panel is wired **per column**: each of the 16 SCT2024 drivers drives one
16-row column, and bit 0…15 of that driver maps to rows top to bottom. The driver
order is interleaved in adjacent pairs, so the component uses the measured
left-to-right column order `7, 15, 6, 14, 5, 13, 4, 12, 3, 11, 2, 10, 1, 9, 0, 8`.
This is different from the old quadrant/row-based mapping, which produced a
half-swapped image. The default orientation assumes the panel is **standing on
its feet** and applies a **90° clockwise** rotation in the mapping. The logical
display is 16×16; the standard `rotation:` option can still be used for
additional adjustments.

## Configuration

`frekvens.yaml` is a complete example:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [frekvens]

display:
  - platform: frekvens
    id: frekvens_panel
    data_pin: GPIO27
    clock_pin: GPIO14
    latch_pin: GPIO19
    enable_pin: GPIO26
    clock_speed: 25000000
    brightness: 10%
    update_interval: 33ms
    lambda: |-
      for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
          it.draw_pixel_at(x, y, Color(0, 0, 0, (x + y) * 255 / 30));
        }
      }
```

When using this repository as a published external component instead of a local
checkout, use `source: github://bernikr/frekvens@main` (with
`components: [frekvens]`).

Options:

| Option | Default | Description |
| :--- | :--- | :--- |
| `data_pin`, `clock_pin`, `latch_pin` | required | Panel control pins. |
| `enable_pin` | none | Active-low LED output enable. Kept off while suspended. |
| `enable_pin_inverted` | `true` | Set to `false` for an active-high enable. |
| `clock_speed` | `10000000` | SPI clock. `25000000` is the SCT2024 maximum and is used in the example. |
| `brightness` | `100%` | Global scale applied to pixel values before BAM. `10%` is a comfortable test level. |
| `update_interval` | `1s` | How often the display lambda is rendered and re-encoded. |

Grayscale comes from the **white** channel of `Color`. RGB colors are converted
to perceived luminance, so existing monochrome display code works unchanged.
Use `Color(0, 0, 0, v)` for direct 8-bit values.

### Brightness in Home Assistant

The example exposes a template number (`number.frekvens_brightness`) with
`restore_value: true`, so the last level survives a reboot. The component also
provides an action:

```yaml
- frekvens.set_brightness: 20%
```

Brightness is applied to the pixel values, so it does not change the panel
refresh rate. The optional `enable_pin` is only a power gate; it is not used for
dimming.

### Demo animations

`frekvens.yaml` includes a `select.frekvens_animation` dropdown with **Auto
demo**, **Plasma**, **Ripple**, **Fire**, **Matrix rain**, **Lissajous**,
**Spiral**, **Warp starfield**, **Column scan**, **Row scan** and **Off**, plus
an **Animation Speed** number. All animation logic lives in the YAML lambda; the
component only exposes the display. Auto demo cycles through the decorative
animations every eight seconds.

**Column scan** and **Row scan** are mapping diagnostics: they light one physical
column left-to-right, or one physical row top-to-bottom, about every 0.5 s at
100 % speed. Use them to check the default orientation and the shift-register
column order.

The display updates at `update_interval: 33ms` (about 30 fps) while the panel
still refreshes its BAM frames at ~78 Hz independently.

## Build and flash

Requires ESPHome 2026.10 or newer (the component uses the modern display and
`gptimer` APIs).

```bash
esphome config frekvens.yaml
esphome compile frekvens.yaml
esphome upload frekvens.yaml --device frekvens.lan
esphome logs frekvens.yaml --device frekvens.lan
```

Define the secrets in `secrets.yaml` (see `secrets.yaml.example`):

```yaml
wifi_ssid: "..."
wifi_password: "..."
frekvens__encryption_key: "..."   # base64 API encryption key
frekvens__ota_password: "..."
```

The device needs an ESPHome OTA password or the API encryption key; both are
kept out of git.

## Diagnostics

Set `logger: level: DEBUG` to see a line every 10 seconds:

```text
BAM: 78 fps, 8 late latch(es), avg wake 9 us, avg SPI 45 us, max wake 25 us, max SPI 225 us, cores isr=1 task=1
Late latches per plane: 8 0 0 0 0 0 0
```

A few late latches per 10 s are harmless (the previous bitplane is shown for one
extra tick). A large number that only appears at plane 0 usually means heavy
WiFi/OTA/flash activity is delaying the SPI transfer; the visible effect is a
very slight variation in the lowest gray level.

## Recovery

The final firmware exposes a **Safe Mode** button (`button.frekvens_safe_mode`).
If an OTA goes wrong, press it from Home Assistant; the device reboots with only
WiFi/API/OTA enabled. While safe mode is active, OTA works without the panel task
running.

`recovery.yaml` is an even smaller image with no display component at all, for
the case where the panel component prevents OTA. It keeps the output-enable pin
inactive so the panel stays dark.

## Credits

* BAM approach and the panel wiring/orientation: Jeroen Domburg
  ([spritesmods.com/?art=frekvens](https://spritesmods.com/?art=frekvens)).
* Original ESPHome experiments in this repository and earlier work by
  `@bernikr`.
