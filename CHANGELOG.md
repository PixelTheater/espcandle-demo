# Changelog

All notable changes to this project will be documented in this file.

This project follows [Semantic Versioning](https://semver.org/). Release entries are grouped by Git tag.

## [Unreleased]

### Added

- Add MIT `LICENSE` file.
- Add this `CHANGELOG.md` to track releases.

### Changed

- Add README frontmatter for documentation indexing.
- Rename the LED configuration image asset to `media/candle-leds-and-features.jpg` and update the README reference.
- Replace the fragile GitHub `issues/new` URL in the README with stable issue-reporting wording.
- Trim README trailing whitespace.

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
