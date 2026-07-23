# ESP32 Candle — Adoption Strategy

**Goal:** turn the candle on, let Home Assistant discover it, add it to HA — never type a Wi-Fi password into a YAML file or copy an encryption key.

This document is the canonical reference for how device discovery and onboarding work for the ESP32 Candle, the security model behind it, and the open decisions still on the table. It is self-contained — the research notes that informed it live outside the repository.

## The four discovery paths, in priority order

### 1. BLE Improv (primary end-user path)

`esp32_improv` advertises an Improv GATT service (`00467768-6228-2272-4663-277478268000`) over BLE 5.0. Home Assistant's Bluetooth integration sees the advertisement, prompts the user to provision Wi-Fi, and sends the SSID/PSK over BLE. The candle joins Wi-Fi and the Improv service stops. No app install, no browser, no USB cable beyond power. This is the path the factory firmware is built around.

### 2. USB-CDC improv_serial (web-flasher path)

`improv_serial` exposes the same Improv protocol over the USB-CDC serial port. A Chromium-based browser at `web.esphome.io` (or the ESPHome Device Builder) can provision Wi-Fi over Web Serial. This is the fallback when BLE is unavailable and the path used during a fresh flash from the browser.

### 3. mDNS on Wi-Fi (HA auto-discovery)

Once the candle is on Wi-Fi, the `mdns` component announces `_esphomelib._tcp.local` on port 6053 with the device name and a `config_hash` TXT record. Home Assistant's ESPHome integration listens for these announcements and surfaces new devices under *Settings → Devices & Services → Discovered*. This is how HA finds the candle *after* it has Wi-Fi.

### 4. Device Builder Adopt (Take Control)

`espcandle.factory.yaml` carries a `dashboard_import: package_import_url:` line pointing at this repo. When the ESPHome Device Builder sees a discovered candle, it can **Take Control** — importing the full config from GitHub so the user can edit and re-flash it from the Device Builder UI.

## How the runtime Noise PSK adoption works

The native API uses the Noise protocol for encryption. ESPHome 2025.10.0 introduced the ability to ship firmware with **no baked-in key** (`api: encryption:` with no `key:`):

1. The device compiles with both `USE_API_PLAINTEXT` and `USE_API_NOISE` defined. It starts with no PSK in flash.
2. On first connect, the device accepts a plaintext API connection and reports `api_encryption_supported = true` in its `DeviceInfoResponse` (proto field 19).
3. Home Assistant (via `aioesphomeapi` ≥ 2025.10.0) generates a fresh 32-byte Noise PSK and sends it to the device using the `noise_encryption_set_key` RPC (proto messages 124/125).
4. The device persists the PSK in flash and disables plaintext. From then on, only encrypted (Noise) connections are accepted. Only a factory reset can remove the key.

This means a **single factory binary** can be flashed to every candle, and each candle ends up with a unique per-device encryption key assigned at adoption. No `secrets.yaml`, no `openssl rand -base64 32`, no per-device YAML edit for the end user.

Upstream references:

- Runtime PSK: <https://github.com/esphome/esphome/pull/7296>
- Wizard auto-generation of a Noise PSK: <https://github.com/esphome/esphome/pull/13634>
- Native API component: <https://esphome.io/components/api/>
- Release where the runtime path landed: <https://esphome.io/changelog/2025.10.0/>
- Improv Wi-Fi standard: <https://www.improv-wifi.com/>

## When to use the dev (static-key) build

The runtime-PSK path requires Home Assistant to be present at adoption time. For offline rigs, CI, or devices that must come up fully configured without an HA round-trip, use the static-key path: copy `esphome/secrets-example.yaml` to `esphome/secrets.yaml` and follow the commented usage block at the bottom of that file to bake a per-device `api.encryption.key` and OTA password into a per-device YAML. This is the only path that uses `secrets.yaml`; the factory and example builds do not.

## Security model

Three risks were considered and a decision recorded for each.

### API — open for one adoption window, then Noise-only

Acceptable. The one-time plaintext window exists only until HA adopts the device and stores a PSK. After that, plaintext is disabled and cannot be re-enabled without a factory reset. The adoption happens on the trusted LAN.

### OTA — open on the LAN forever (open question)

There is **no runtime OTA-password adoption flow** in ESPHome 2026.6.x. The factory firmware's `ota:` block has no password, so anyone on the LAN can push firmware to the candle for the lifetime of the device. Mitigation in place: OTA requires LAN access, and the base exposes **Safe Mode** and **Factory Reset** buttons in Home Assistant for recovery. If a project-wide OTA password is later required, it must be baked into the factory binary — which breaks the single-firmware goal. This is recorded as an open question below; the current decision is to accept the open-OTA risk on the trusted-LAN model.

### Fallback AP — open, captive-portal only

`wifi: ap: {}` is an open access point that comes up only if Wi-Fi fails (default after 90s). It serves only the captive-portal provisioning page. Accepted per the user decision for the retail path.

### Web server — off by default (open question)

The factory firmware does **not** enable `web_server`. Home Assistant is the primary UI. Modders can uncomment the block in `espcandle.base.yaml` (see `docs/modders.md`); it ships without `auth:` (trusted-LAN). Alternatives if re-enabled: add `web_server: auth:`, or leave it out of the factory path. Recorded as an open question below.

## Versioning

`project: version` is single-sourced in `esphome/espcandle.base.yaml` (currently `0.4.0`, matching the GitHub Release / support-site version). The factory YAMLs inherit it via `packages:` and do not redeclare it. `esphome/manifest.json` is a skeleton; HTTP self-update on the factory flash image is **disabled** until a release script populates the manifest and hosts `firmware.ota.bin`. Bumping the version is a two-edit change at release time: the `project_version` substitution in the base, and the `version` field in the manifest. The support-site web flasher uses the GitHub Release tag — keep that tag and `project_version` identical (see `docs/support-site-release.md`).

## Open questions

1. **Button-based Improv authorizer for dev units** — `esp32_improv: authorizer: <binary_sensor>` (e.g. 3-second boot-button hold) instead of `none`, for development devices where open BLE provisioning is undesirable.
2. **Static fallback AP password for retail** — set a `!secret`/printed password on the fallback AP instead of leaving it open.
3. **Hosted manifest URL** — serve `manifest.json` (and the factory binary) from a project-owned URL (`support.soliddifference.com`) rather than `raw.githubusercontent.com`, for stable retail update URLs.
4. **Runtime OTA password** — waiting on upstream ESPHome support; until then, OTA remains open on the LAN.
5. **GPIO10-12 pin-safety review** — the switches for GPIO10/11/12 are commented out in the base; before enabling them for modders, confirm they are safe GPIO on the candle's ESP32-S3 module (not SPI flash / strapping). Tracked as housekeeping item TD-015.
6. **`web_server` decision** — leave commented out (current), enable with `auth:`, or enable open on trusted LAN. See the security model above.
