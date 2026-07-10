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

Run all `make` targets from the `esphome/` directory, pointing `ESPHOME` at the venv binary:

```bash
cd esphome
ESPHOME=../.venv/bin/esphome make smoke-test
```

## 1. Automated smoke test (no hardware)

```bash
cd esphome
ESPHOME=../.venv/bin/esphome make smoke-test
```

What it does:

1. `esphome config` on all four YAMLs (`base`, `example`, `factory`, `factory.factory`) — schema validation.
2. `esphome compile espcandle.factory.factory.yaml` — full build, including the custom `candle_effects.h` engine and the `partition` light.

A pass means: the YAML is schema-valid and the firmware (provisioning + partition light + custom effects + effect-cycling button) compiles. It does **not** prove runtime behavior — that needs a real candle (below).

This is the gate to run before pushing a branch or opening a PR. It runs in ~90s on a warm build.

## 2. First flash on a real candle

```bash
cd esphome
ESPHOME=../.venv/bin/esphome make factory-run
```

`factory-run` = `config` + `compile` + `run`. On first run it downloads ESP-IDF (~5 min, cached after). When it asks for a port, pick the `/dev/cu.usbmodem*` entry that appeared when you plugged the candle in.

Watch the serial monitor for a clean boot:

```
[I][app:xxx] Booting esphome.io...
[I][wifi:xxx] WiFi connected.            # only after provisioning
```

### Known boot-loop risk (2026.6.x + esp32_rmt_led_strip)

There is a reported issue ([esphome#17060](https://github.com/esphome/esphome/issues/17060)) where 2026.6.x images can boot-loop on some `esp32_rmt_led_strip` configs. If the device boot-loops after flash:
- Unplug power, then replug — sometimes recovers.
- If it persists, OTA rollback (default on) should revert to the previous image. If there is no previous image, reflash via USB.
- Capture the serial log and check the issue for a fix; you may need to pin to 2026.6.0 or a later patch.

This is why we validate with `make smoke-test` first and keep `factory-run` as a deliberate, observed step.

## 3. Provisioning + HA adoption checklist

After a clean boot, the candle is unprovisioned (no Wi-Fi). Adopt it:

1. **Provision Wi-Fi** — either:
   - **BLE Improv**: in Home Assistant, the candle appears under *Settings → Devices & Services* as a new ESPHome device; tap to send Wi-Fi credentials over Bluetooth, **or**
   - **USB-CDC Improv**: visit `web.esphome.io` in Chrome/Edge, connect over USB-C, send Wi-Fi credentials, **or**
   - **Captive portal**: if Wi-Fi fails for ~90s the candle opens a fallback AP — join it and submit credentials at the captive portal page.
2. **mDNS discovery** — once on Wi-Fi, the candle announces `_esphomelib._tcp.local`. Home Assistant shows it under *Discovered*.
3. **Adopt** — tap **Configure** in HA. The runtime Noise PSK is generated and stored on the candle automatically (no key to paste).
4. **Entities** — you should see exactly one light: **ESP Candle**. The five physical channel lights are internal (hidden). Recovery buttons: Restart, Safe Mode, Factory Reset. Diagnostics are present but `disabled_by_default` — enable in HA if you want them.

## 4. Effect smoke test (on-device)

The `ESP Candle` light has three custom effects: **Candle**, **Color**, **Magic**.

1. Turn the light on from HA. Default (no effect) should be... nothing special (partition at rest). Set the effect to **Candle** from the HA dropdown.
2. **Candle**: warm whites flicker organically (Perlin noise), red ember glows inverse to flame brightness, RGB strip off. The flicker should be slow/calm sometimes and faster/windier at others (sub-modes cycle every 4-20s).
3. **Color**: RGB strip shows a slowly drifting hue spread; the drift speed itself wanders between near-static and fast-cycling. Whites/UV/red off.
4. **Magic**: RGB strip alternates between a dim purple/blue drift and bright sparks; UV channel on; red LED drifts slowly and independently. Whites off.
5. **Button cycling**: short-press the boot button — the effect should advance Candle → Color → Magic → Candle → …. Long-press (3s) toggles the candle on/off.

### If an effect looks wrong

The custom engine lives in `esphome/includes/candle_effects.h`. Tunables are grouped per effect (`candle::`, `color::`, `magic::` namespaces), mirroring `src/config.h`. Adjust and re-run `make factory-run`. Common issues:

- **Too dim / too bright** — adjust `brightness` (per sub-mode) or the `max_power` on the `ledc` outputs in the YAML.
- **Flicker too fast/slow** — adjust `speed`/`speed2`/`smoothing` in the relevant sub-mode `SubParams`.
- **Red ember too strong** — adjust `RED_MIN` / `RED_MAX` / `RED_CURVE`.
- **Effect not applying to a channel** — check the `id(pwm_led_N)` references and that the channel light is `internal: true` (so HA isn't fighting the effect).

## 5. OTA update test

Once adopted, an OTA update over Wi-Fi should work:

```bash
cd esphome
ESPHOME=../.venv/bin/esphome esphome run espcandle.factory.factory.yaml --device espcandle-XXXXXX.local
```

(substitute the device's mDNS name). The update uses the base's `ota: - platform: esphome` (no password — open on the trusted LAN; see `docs/adoption-strategy.md` security model).

## 6. What's not (yet) automated

- **Hardware-in-the-loop**: no automated rig to flash, power-cycle, and assert serial markers or LED output. The `make smoke-test` target is config+compile only. A future CI step could at least run `make smoke-test` on every push.
- **Effect visual regression**: there's no way to assert LED output without a camera or photodiode rig. Effect validation is manual (section 4).
- **ESP-IDF / RMT boot-loop repro**: tracked upstream as #17060; not reproducible without the specific hardware.

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
      - run: cd esphome && make smoke-test
```

This would catch config/compile regressions (including the custom effect engine) without hardware. The on-device runbook above remains manual.
