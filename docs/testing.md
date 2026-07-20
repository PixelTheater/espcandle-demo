# ESP Candle — Testing & Validation Runbook

How to validate the ESPHome firmware, with and without hardware. The automated `make smoke-test` covers the no-hardware path; the rest is the on-device runbook for testing a real candle.

## Prerequisites

- Python **3.13** (not 3.14 — ESPHome 2026.6.x deps don't all build on 3.14 yet).
- A project venv with ESPHome installed:

  ```bash
  python3.13 -m venv .venv
  source .venv/bin/activate
  pip install -r esphome/requirements.txt   # pins esphome>=2026.6.0,<2027.0.0
  ```
- (For the on-device steps) an ESP32-S3 candle plugged in over USB-C.

Run `make` from the **repo root**. The root Makefile forwards into `esphome/` and defaults `ESPHOME` to `.venv/bin/esphome`.

```bash
make smoke-test
```

## 1. Automated smoke test (no hardware)

```bash
make smoke-test
```

What it does:

1. `esphome config` on all four YAMLs (`base`, `example`, `factory`, `factory.factory`) — schema validation.
2. `esphome compile espcandle.factory.factory.yaml` — full build, including the custom `candle_effects.h` engine, Preset select, and channel number entities.

A pass means: the YAML is schema-valid and the firmware (provisioning + RGB light + custom effects + preset-cycling button) compiles. It does **not** prove runtime behavior — that needs a real candle (below).

This is the gate to run before pushing a branch or opening a PR. It runs in ~90s on a warm build.

## 2. First flash on a real candle

Close any PlatformIO / serial monitor holding the USB port, then:

```bash
make factory-run
```

`factory-run` = `config` + `compile` + `run`. On first run it may download ESP-IDF tooling (cached after). When it asks for a port, pick the `/dev/cu.usbmodem*` entry for the candle (USB JTAG/serial debug unit).

Watch the serial monitor for a clean boot:

```
[I][app:xxx] Booting esphome.io...
[I][wifi:xxx] WiFi connected.            # only after provisioning
```

### First-flash tips

Watch the serial monitor through the first boot. If the device fails to stay up:
- Unplug power, then replug — USB-CDC ports sometimes need a re-enumeration.
- OTA rollback (default on) reverts to the previous image when a new build crashes repeatedly; on a blank chip there is no previous image, so reflash via USB.
- Capture the serial log. Heap exhaustion (not LED-strip-specific) has caused boot loops on other 2026.6.x devices under heavy BLE/Wi-Fi load; this firmware is much lighter, but a log still helps.

Validate with `make smoke-test` first; keep `factory-run` as a deliberate, observed step.

## 3. Provisioning + HA adoption checklist

After a clean boot, the candle is unprovisioned (no Wi-Fi). Adopt it:

1. **Provision Wi-Fi** — either:
   - **BLE Improv**: in Home Assistant, the candle appears under *Settings → Devices & Services* as a new ESPHome device; tap to send Wi-Fi credentials over Bluetooth, **or**
   - **USB-CDC Improv**: visit `web.esphome.io` in Chrome/Edge, connect over USB-C, send Wi-Fi credentials, **or**
   - **Captive portal**: if Wi-Fi fails for ~90s the candle opens a fallback AP — join it and submit credentials at the captive portal page.
2. **mDNS discovery** — once on Wi-Fi, the candle announces `_esphomelib._tcp.local`. Home Assistant shows it under *Discovered*.
3. **Adopt** — tap **Configure** in HA. The runtime Noise PSK is generated and stored on the candle automatically (no key to paste).
4. **Entities** — one light (**ESP Candle**), a **Preset** select, and config numbers for White / Red / UV. Recovery buttons: Restart, Safe Mode, Factory Reset. **Internal Temperature** is enabled; other diagnostics are `disabled_by_default`.

## 4. Effect / preset smoke test (on-device)

The `ESP Candle` light has three custom effects: **Candle**, **Color**, **Magic**. Prefer the **Preset** dropdown (or boot short-press) over raw effect picks.

1. Turn the light on from HA with Preset **Off** — it should start **Candle** without a bright white flash. The light toggle stays the master off switch.
2. **Candle**: warm whites flicker organically (Perlin noise), red ember glows inverse to flame brightness, RGB strip off. Sub-modes cycle every 4–20s.
3. **Color**: RGB strip shows a slowly drifting hue spread; whites/UV/red off.
4. **Magic**: RGB strip alternates dim purple/blue drift and brighter sparks; UV on; red drifts; whites off.
5. **Presets**: try Warm Glow (dim static; light entity stays ON) / Ember (slow red flicker) / UV Accent. Light toggle Off should extinguish everything and set Preset to Off. Moving a channel slider should flip Preset to **Custom**.
6. **Button**: short-press cycles Warm Glow → Candle → Color → Magic → …. Hold 3–10s then release to toggle on/off. Hold 10s to factory-reset.

### If an effect looks wrong

The custom engine lives in `esphome/includes/candle_effects.h`. Tunables are grouped per effect (`candle::`, `color::`, `magic::` namespaces), mirroring `src/config.h`. Adjust and re-run `make factory-run`. Common issues:

- **Too dim / too bright** — adjust `brightness` (per sub-mode) or the `max_power` on the `ledc` outputs in the YAML.
- **Flicker too fast/slow** — adjust `speed`/`speed2`/`smoothing` in the relevant sub-mode `SubParams`.
- **Red ember too strong** — adjust `RED_MIN` / `RED_MAX` / `RED_CURVE`.
- **Effect not applying to a channel** — check `id(pwm_led_N)` in the effect header; only effects / Preset / numbers should call `set_level` (no competing light wrappers).

## 5. OTA update test

Once adopted, an OTA update over Wi-Fi should work:

```bash
.venv/bin/esphome run esphome/espcandle.factory.factory.yaml --device espcandle-XXXXXX.local
```

(substitute the device's mDNS name; run from repo root). Uses the base's `ota: - platform: esphome` (no password — open on the trusted LAN; see `docs/adoption-strategy.md`).

## 6. What's not (yet) automated

- **Hardware-in-the-loop**: no automated rig to flash, power-cycle, and assert serial markers or LED output. The `make smoke-test` target is config+compile only. A future CI step could at least run `make smoke-test` on every push.
- **Effect visual regression**: there's no way to assert LED output without a camera or photodiode rig. Effect validation is manual (section 4).
- **First-boot / heap issues under load**: rare; needs serial capture on real hardware.

## CI hook (future)

A minimal GitHub Action to run the smoke test on every PR:

```yaml
# .github/workflows/esphome-smoke.yml  (not yet created)
on: [push, pull_request]
jobs:
  smoke:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with: { python-version: "3.13" }
      - run: pip install -r esphome/requirements.txt
      - run: make smoke-test
```

This would catch config/compile regressions (including the custom effect engine) without hardware. The on-device runbook above remains manual.
