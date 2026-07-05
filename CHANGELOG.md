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

### Changed

- Add README frontmatter for documentation indexing.
- Rename the LED configuration image asset to `media/candle-leds-and-features.jpg` and update the README reference.
- Replace the fragile GitHub `issues/new` URL in the README with stable issue-reporting wording.
- Trim README trailing whitespace.
- Pin the ESPHome version used by the Makefile to `esphome@2025.7.3` (a real, tested release) instead of the previous `esphome@2026.4` placeholder.
- Tighten the `esphome/requirements.txt` range to `>=2025.7.0,<2026.0.0` to match the Makefile default and the `min_version` declared in `esphome/espcandle.base.yaml`.
- Refresh `esphome/secrets-example.yaml` with explicit comments, rotation guidance, and a clear per-device naming convention.

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
