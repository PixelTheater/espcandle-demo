# Changelog

All notable changes to this project will be documented in this file.

This project follows [Semantic Versioning](https://semver.org/). Release entries are grouped by Git tag.

## [Unreleased]

## [0.5.0] - 2026-08-09

### Added

- Per-channel **White / Red / UV** enable switches alongside the level sliders.
- **RGB Strip** switch: gates WS2812 output only; colour is preserved when off.

### Changed

- Control model: scene presets own all outputs. Any user change (slider,
  channel enable, or selecting Custom) leaves the scene for **Custom** with
  independent channels. White/Red/UV use level × on/off as absolute PWM (not
  gated by the RGB light).
- Turning the main light off in Custom turns off RGB only (PWM stays). Preset
  Off still extinguishes everything. Raising UV/Red/White from Off lights that
  channel without requiring the RGB light on.
- Light on from Preset **Off** enters **Custom** (manual), not Candle.

### Fixed

- Template number/switch lag (`set_action` before optimistic publish) so
  sliders and enables apply the intended value (`x` / explicit publish).
- RGB Strip no longer stops scene effects or zeroes the light colour (that made
  the picker look dead and broke Candle/Color when toggling the strip).
- Enabling RGB Strip from Preset Off now flips to Custom (same handoff as
  powering the main light from Off).
- Scene entry publishes channel enables/levels that match what the effect
  drives (e.g. Magic: Red+UV on; Candle: White+Red on), so toggles are not
  fighting a lying UI. Leaving a scene stops the effect and turns RGB Strip
  off (no solid-white jump at scene brightness); enable toggles in Custom no
  longer re-stop the strip.
## [0.4.0] - 2026-07-23

### Added

- **Wifi RSSI** preset: steady diagnostic gauge — white level tracks absolute
  WiFi signal, red kicks in when weak, RGB glows green when strong. Default
  master 70%. Boot short-press includes it in the cycle after Candle.

### Changed

- Candle mode: WiFi RSSI *deltas* bias wind/snuff lightly; absolute level does
  not dim the normal Candle preset.

## [0.3.0] - 2026-07-22

### Added

- **Natural** preset/effect: static warm whites + soft orange-white RGB tint;
  scales with the master brightness slider.
- Warm Glow and UV Accent as static scene effects (not one-shot PWM writes) so
  they track the master slider like other presets.
- Master-brightness architecture: each producing preset sets a default master
  % (Candle 40%, Color 50%, Natural 50%, Magic/Ember 45%, Warm Glow 60%,
  UV Accent 40%) instead of forcing 100%.

### Changed

- Candle animation: mid-band value noise + hermite smoothstep, white peaks
  aligned with standalone (`÷ white_max_power`), time-based soft snuffs,
  exponential smoothing, 20 ms tick; white PWM frequency 5 kHz (was 300 Hz).
- Boot button cycle: Warm Glow → Natural → Candle → Color → Magic.
- On static scenes (Warm Glow / Natural / UV Accent), White / Red / UV sliders
  tweak mix in place without leaving the preset or changing master / RGB tint.
- On animated scenes (Candle / Color / Magic / Ember), channel sliders still
  flip Preset to **Custom** and stop the scene effect; Custom remasters PWM
  from number states when brightness changes.

### Fixed

- Moving UV / White on UV Accent or Natural no longer stops the scene and
  drops RGB (was flipping to Custom and clearing the strip tint).
- Preset entry no longer forces brightness to 100% (the “shoots up” jump).

## [0.2.0] - 2026-07-20

### Added

- Add MIT `LICENSE` file.
- Add this `CHANGELOG.md` to track releases.
- Seed the firmware RNG in `setup()` using chip-unique entropy so candle, color, magic, and auto-mode randomness differs across reboots.
- Gate the `[CANDLE]` diagnostic log in `updateCandleMode()` behind a `CANDLE_DEBUG_LOG` macro (default 0) to keep production builds quiet.
- Add `dutyToFastLEDBrightness()` helper and use it from `setChannelDuty`, `testRamp`, `testRand`, `testHold`, `testRgbRainbow`, and `testRgbChase` so 0-4095 PWM duty values are scaled correctly to FastLED's 0-255 range and no longer wrap modulo 256.
- Add a bounds check in `setPWMBrightness()` so out-of-range channel indices are rejected before indexing `LED_PINS[]`.
- Ignore the audit/agent scratch directory `.pi/` in `.gitignore`.
- Lean ESPHome factory YAML: one user-facing `ESP Candle` RGB light with
  Candle / Color / Magic effects; Preset select; White / Red / UV config
  numbers driving LEDC directly (no partition light).
- Boot button: short press cycles presets (Warm Glow → Candle → Color →
  Magic); hold 3–10s then release to toggle; hold 10s to factory-reset.
- `docs/modders.md`: I2C, extra GPIO 10/11/12, web server, channel control,
  and the static-key dev path.
- Custom ESPHome effect engine: port the standalone Candle/Color/Magic
  algorithms into `esphome/includes/candle_effects.h` (Perlin-noise flame,
  HSV drift+history, drift/spark+UV) wired as `addressable_lambda`
  effects on the RGB light. Self-contained value-noise (no FastLED
  dependency) since `esp32_rmt_led_strip` is not FastLED-backed.
- `make smoke-test` Makefile target: validates all four YAMLs and compiles
  the factory binary (no hardware required). PR gate.
- GitHub Actions CI (`.github/workflows/ci.yml`): ESPHome smoke-test +
  PlatformIO demo build on push/PR.
- `docs/testing.md`: full validation runbook (automated smoke test +
  on-device flash/provision/adopt/effect checks + known boot-loop risk).
- `docs/support-site-release.md` and `.github/workflows/publish-firmware.yml`:
  publish merged ESPHome + Standalone bins to GitHub Releases and notify
  support.soliddifference.com for browser flashing.

### Changed

- Align ESPHome `project_version` with the release tag (`0.2.0`); previously
  the internal architecture label was `1.6.0`.
- Disable HTTP self-update on the factory flash image until a release
  hosts `manifest.json` + OTA binary (stops boot-time 404 noise). HA
  ESPHome OTA remains available.
- ESPHome architecture A: drop partition + monochromatic channel
  lights; PWM owned by effects / Preset / numbers via `set_level`. Power
  caps aligned with standalone (`white` 40%, `uv` 75%, `red` 80%). Magic
  sparks brighter (higher chance, slower fade, occasional double spark).
  Internal Temperature enabled by default.
- Bump ESPHome pin to `esphome@2026.6.5` (Makefile) and `>=2026.6.0,<2027.0.0`
  (requirements.txt); `min_version: 2026.6.0` in the base.
- Single-firmware runtime-PSK adoption path: `api:` with no baked-in key;
  `espcandle.factory.yaml` / `espcandle.factory.factory.yaml`;
  `manifest.json` skeleton; secrets file for the static-key dev path only.
  See `docs/adoption-strategy.md`.
- PlatformIO: project-local `boards/espcandle.json` (N4R2 flash/PSRAM
  profile, product name). Pin `platform` to pioarduino 55.03.39
  (Arduino-ESP32 3.x) so CI matches local builds (`ledcAttach`).
  ESPHome: stock `esp32-s3-devkitc1-n4r2` for the same memory profile.
  Pinout is candle-specific.
- README / docs sync: make targets run from `esphome/`; Magic mode and UV
  count; web flasher status; adoption-strategy matches lean YAML
  (web_server and GPIO10–12 commented out).
- Add README frontmatter for documentation indexing.
- Rename the LED configuration image asset to `media/candle-leds-and-features.jpg` and update the README reference.
- Replace the fragile GitHub `issues/new` URL in the README with stable issue-reporting wording.
- Trim README trailing whitespace.

### Removed

- Remove the orphan `esphome/espcandle.yaml` (a leftover from the base/example refactor; not referenced by the README or the Makefile).
- Remove ESPHome partition light and internal monochromatic channel lights
  (replaced by RGB light + Preset + LEDC number entities).

## [0.1.0] - 2026-05-04

### Added

- Add shared ESPHome base configuration at `esphome/espcandle.base.yaml`.
- Add per-device ESPHome template at `esphome/espcandle.example.yaml`.
- Add ESPHome Python requirements at `esphome/requirements.txt`.
- Add build, flash, provisioning, adoption, OTA update, and multi-candle usage instructions to the README.

### Changed

- Refactor ESPHome setup from per-device YAML files into a shared base plus small per-device overlays.
- Update README installation and usage documentation for the current ESPHome workflow.
- Update secrets example for per-device API keys and OTA passwords.

### Removed

- Remove older per-candle ESPHome example/config files that were superseded by the shared base and per-device template.

[Unreleased]: https://github.com/PixelTheater/espcandle-demo/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/PixelTheater/espcandle-demo/releases/tag/v0.5.0
[0.4.0]: https://github.com/PixelTheater/espcandle-demo/releases/tag/v0.4.0
[0.3.0]: https://github.com/PixelTheater/espcandle-demo/releases/tag/v0.3.0
[0.2.0]: https://github.com/PixelTheater/espcandle-demo/releases/tag/v0.2.0
[0.1.0]: https://github.com/PixelTheater/espcandle-demo/releases/tag/v0.1.0
