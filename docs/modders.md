# ESP Candle — Modder & Expansion Guide

This document covers the optional hardware features of the ESP Candle PCB that are **not** enabled in the factory firmware by default: the I2C bus, the extra GPIO switches, the local web server, and SNTP/Last-Restart. These are aimed at tinkerers and modders who fork the YAML.

The factory firmware (`esphome/espcandle.factory.factory.yaml`) intentionally exposes a single user-facing light ("ESP Candle") and a minimal diagnostic set. Everything below is opt-in.

## Where things live

| Feature | Default state | How to enable |
|---|---|---|
| I2C bus (GPIO8/GPIO9) | Commented out | Uncomment the `i2c:` block in `espcandle.base.yaml` |
| Extra GPIO 10/11/12 switches | Commented out | Uncomment the `switch:` block in `espcandle.base.yaml` |
| Local web server | Commented out | Uncomment the `web_server:` block at the bottom of `espcandle.base.yaml` |
| SNTP + Last Restart | Commented out | Uncomment the `time:` + `text_sensor:` template blocks |
| Direct per-channel light control | Internal (hidden) | Remove `internal: true` from the channel lights |
| Static API key / OTA password | Not used (runtime PSK) | See `esphome/secrets-example.yaml` |

## I2C bus

The PCB breaks out I2C on GPIO8 (SDA) and GPIO9 (SCL) via a JST SH 1.0mm QWIIC connector and PCB headers. No I2C devices are configured by default.

```yaml
i2c:
  sda: ${i2c_sda_pin}
  scl: ${i2c_scl_pin}
  scan: true
```

After enabling, add sensors under `sensor:` (e.g. `bme280`, `sht3x`, `tsl2561`). The `scan: true` option logs discovered I2C addresses at boot — useful for bring-up.

## Extra GPIO 10 / 11 / 12

Three GPIO pins are exposed as controllable switches in the default config (commented out). These are intended for expansion modules, fans, or external indicators.

> **⚠️ Pin-safety note.** On some ESP32-S3 modules, GPIO10-12 are connected to the SPI flash interface. Driving them as outputs can conflict with flash access on those modules. Verify your specific module's datasheet before enabling. The ESP32-S3-DevKitC-N8R2 (the candle's reference board) exposes these as safe GPIO, but OEM modules vary.

```yaml
switch:
  - platform: gpio
    pin: GPIO10
    name: "Extra GPIO 10"
    id: extra_gpio_10
    restore_mode: ${switch_restore_mode}
  - platform: gpio
    pin: GPIO11
    name: "Extra GPIO 11"
    id: extra_gpio_11
    restore_mode: ${switch_restore_mode}
  - platform: gpio
    pin: GPIO12
    name: "Extra GPIO 12"
    id: extra_gpio_12
    restore_mode: ${switch_restore_mode}
```

## Local web server

The factory firmware does not include a web server — Home Assistant is the primary UI. If you want a standalone `http://<device>.local` control surface, uncomment:

```yaml
web_server:
  port: 80
  # auth:
  #   username: !secret web_username
  #   password: !secret web_password
```

By default the web server is unauthenticated (trusted-LAN model). If you enable it on a network you don't fully trust, add `auth:` with credentials stored in `secrets.yaml`.

## Direct per-channel control

The factory firmware marks the five physical channel lights `internal: true` so they drive the hardware but don't appear in Home Assistant. Only the `ESP Candle` partition light is exposed.

If you want direct per-channel control (e.g. for a custom dashboard or to drive the whites and RGB independently), remove `internal: true` from any of:

- `rgb_strip` — the 20-LED WS2812 strip
- `white_1`, `white_2` — warm white channels
- `uv_led` — 365nm UV
- `red_led` — deep red

You may also want to add `name:` back to each so they appear with friendly names in HA.

## SNTP time sync + Last Restart

The factory firmware omits the SNTP time component and the "Last Restart" template text sensor to keep the config lean. HA already tracks device availability; if you want a local clock or a "last restarted at" entity, uncomment the `time:` block and the `text_sensor:` template at the bottom of `espcandle.base.yaml`.

## Static API key & OTA password (dev/CI path)

The factory firmware uses runtime Noise-PSK adoption — no key is baked in, and Home Assistant negotiates one on first adoption. For a fully offline build with no HA round-trip (CI, isolated rigs), use the static-key path documented in `esphome/secrets-example.yaml`. That path bakes a per-device `api.encryption.key` and OTA password into a per-device YAML via `secrets.yaml`.

## Forking the factory firmware

Modders who want a substantially different config should fork `espcandle.factory.yaml` (the Device Builder import target) or `espcandle.base.yaml` (the hardware base) rather than editing the factory flash image directly. The `dashboard_import:` line in `espcandle.factory.yaml` points at this repo — if you fork, update that URL to your own repo so the Device Builder's "Take Control" flow imports your config.
