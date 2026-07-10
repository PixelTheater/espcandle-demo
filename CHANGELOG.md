# Changelog

All notable changes to this project will be documented in this file.

This project follows [Semantic Versioning](https://semver.org/). Release entries are grouped by Git tag.

## [Unreleased]

### Added

- Add MIT `LICENSE` file.
- Add this `CHANGELOG.md` to track releases.
- Seed the firmware RNG in `setup()` using chip-unique entropy so candle, color, magic, and auto-mode randomness differs across reboots.
- Gate the `[CANDLE]` diagnostic log in `updateCandleMode()` behind a `CANDLE_DEBUG_LOG` macro (default 0) to keep production builds quiet.
- Add `dutyToFastLEDBrightness()` helper and use it from `setChannelDuty`, `testRamp`, `testRand`, `testHold`, `testRgbRainbow`, and `testRgbChase` so 0-4095 PWM duty values are scaled correctly to FastLED's 0-255 range and no longer wrap modulo 256.
- Add a bounds check in `setPWMBrightness()` so out-of-range channel indices are rejected before indexing `LED_PINS[]`.
- Ignore the audit/agent scratch directory `.pi/` in `.gitignore`.
- Lean ESPHome factory YAML: expose one user-facing `ESP Candle` partition light
  (RGB strip + warm white 1/2 + UV + deep red) with curated effects
  (Candle / Color / Magic); mark the five underlying channel lights
  `internal: true`; hide noisy diagnostics with `disabled_by_default: true`.
- Boot button: short press cycles effects, long press toggles the candle
  on/off (uses a `globals` counter + `light.turn_on` lambda because
  `light.effect.next` requires ESPHome >= 2026.6.0 and this firmware pins
  the 2025.10/2025.12 line).
- `docs/modders.md`: I2C, extra GPIO 10/11/12, web server, direct
  per-channel control, and the static-key dev path.
- Custom ESPHome effect engine: port the standalone Candle/Color/Magic
  algorithms into `esphome/includes/candle_effects.h` (Perlin-noise flame,
  HSV drift+history, drift/spark+UV) wired as `addressable_lambda`
  effects on the `ESP Candle` partition light. Self-contained value-noise
  (no FastLED dependency) since `esp32_rmt_led_strip` is not FastLED-backed.
- `make smoke-test` Makefile target: validates all four YAMLs and compiles
  the factory binary (no hardware required). PR gate.
- `docs/testing.md`: full validation runbook (automated smoke test +
  on-device flash/provision/adopt/effect checks + known boot-loop risk).

### Changed

- Bump ESPHome pin to `esphome@2026.6.5` (Makefile) and `>=2026.6.0,<2027.0.0`
  (requirements.txt); `min_version: 2026.6.0` in the base. Unblocks
  `light.effect.next` for the button.
- Boot button effect cycling simplified to `light.effect.next` (was a
  `globals` counter + `light.turn_on` lambda, needed because the action
  landed in ESPHome 2026.6.0 and the firmware previously pinned 2025.12).
  - `esphome/espcandle.base.yaml`: `api:` now ships with no baked-in
    key by default; Home Assistant negotiates a per-device Noise PSK
    at adoption (requires ESPHome >= 2025.10.0). Bumped `min_version`
    to 2025.10.0 and `project_version` to 1.5.0.
  - `esphome/espcandle.example.yaml`: rewritten as a per-device named
    overlay with no secrets; `name_add_mac_suffix: false`.
  - Added `esphome/espcandle.factory.yaml` (Device Builder import
    target) and `esphome/espcandle.factory.factory.yaml` (bench-flash
    binary with `http_request` OTA self-update).
  - Added `esphome/manifest.json` skeleton (populated at release time).
  - `esphome/secrets-example.yaml`: repurposed for the dev-only
    static-key path.
  - `esphome/Makefile` + `requirements.txt`: bumped ESPHome pin to
    2025.10.5 and the range to `>=2025.10.0,<2026.0.0`; added
    `factory`, `factory-run`, `factory-config` targets.
  - See `docs/adoption-strategy.md` for the full strategy and security
    model.
- Add README frontmatter for documentation indexing.
- Rename the LED configuration image asset to `media/candle-leds-and-features.jpg` and update the README reference.
- Replace the fragile GitHub `issues/new` URL in the README with stable issue-reporting wording.
- Trim README trailing whitespace.

### Removed

- Remove the orphan `esphome/espcandle.yaml` (a leftover from the base/example refactor; not referenced by the README or the Makefile).

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

[Unreleased]: https://github.com/PixelTheater/espcandle-demo/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/PixelTheater/espcandle-demo/releases/tag/v0.1.0
