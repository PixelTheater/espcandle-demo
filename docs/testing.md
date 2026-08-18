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
2. `esphome compile espcandle.factory.factory.yaml` — full build, including the custom `candle_effects.h` engine, Preset select, channel numbers/enables, and RGB Strip.

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
4. **Entities** — one light (**ESP Candle** = RGB strip on/off + colour + brightness in Custom; scene master in presets), a **Preset** select, White/Red/UV level numbers + enable switches, and **RGB Strip**. Recovery buttons: Restart, Safe Mode, Factory Reset. **Internal Temperature** is enabled; other diagnostics are `disabled_by_default`. After a power cycle or firmware reboot the candle is Preset **Off** (all outputs dark) — last scene / Custom mix is not restored, so a brownout from high PWM cannot loop.

## 4. Effect / preset smoke test (on-device)

The `ESP Candle` light has scene effects: **Warm Glow**, **Natural**, **Ember**, **UV Accent**, **Candle**, **Wifi RSSI**, **Color**, **Magic**. Prefer the **Preset** dropdown (or boot short-press) over raw effect picks.

1. Turn the light on from HA with Preset **Off** — it should enter **Custom** (manual) on the first attempt; the strip stays lit. PWM stays at the current slider/enable state; RGB follows the light.
2. **Candle**: warm whites flicker organically, red ember inverse to flame; RGB off. Master slider dims the whole scene; slider should sit near 40% on entry. Nearby RF motion may gently nudge wind/snuff.
3. **Wifi RSSI**: steady white level tracks absolute WiFi signal (−88 dBm ≈ dim, −42 dBm ≈ bright). Red fades in below ~40% signal; RGB strip glows green above ~55%. No flicker. Master defaults to ~70%.
4. **Color**: HSV drift on the strip; entry brightness ~50%. Master slider scales the strip.
5. **Natural**: static warm whites + soft orange-white RGB, no animation; entry ~50%. Moving the master slider scales both proportionally.
6. **Presets vs Custom**: while a scene is selected it owns outputs. Moving a White/Red/UV slider or enable, or choosing **Custom**, leaves the scene. In Custom each channel is independent: level × on/off (absolute PWM, not gated by the RGB light). **RGB Strip** gates WS2812s only (colour is preserved when off). Light toggle Off in Custom turns off RGB only; from a scene it extinguishes everything and selects Preset Off.
7. **Custom channels**: Preset Off or Custom → raise UV (or Red/White) alone — that channel should light immediately without requiring the RGB light on. Setting a slider to 0 or toggling the channel enable off must extinguish that channel. Colour picker works when the light / RGB Strip is on.
8. **Regression — independent PWM**: Custom → turn the main light off → raise **UV Intensity** — UV should still light. Toggle **RGB Strip** off/on — strip goes dark/lit without stopping White/Red/UV.
9. **Regression — Off + RGB Strip**: Preset **Off** → turn **RGB Strip** on — strip should light on the first attempt and Preset must flip to **Custom** (not stay Off).
10. **Regression — Magic channel UI**: select **Magic** — **Red** and **UV** enables should be on (≈40% / ≈55%), **RGB Strip** on, White off. Toggling **Red** off must leave for **Custom**, stop Magic (strip dark, **RGB Strip** off — not solid white), extinguish red PWM, and leave UV at its level if still enabled.
11. **Button**: short-press cycles Warm Glow → Natural → Candle → Wifi RSSI → Color → Magic → …. Hold 3–10s then release to toggle on/off. Hold 10s to factory-reset.
12. **Regression — boot always Off**: with a scene running (e.g. Magic at high UV), Restart from HA (or power-cycle). After boot, Preset must be **Off**, all channels dark. HA sliders/enables should read 0 / off. Then repeat item 1 (Off → light on → Custom, strip lit).

### If an effect looks wrong

The custom engine lives in `esphome/includes/candle_effects.h`. Tunables are grouped per effect (`candle::`, `color::`, `magic::`, `natural::`, …), mirroring `src/config.h`. Adjust and re-run `make factory-run`. Common issues:

- **Too dim / too bright overall** — use the **ESP Candle** brightness slider (presets set a default; they no longer force 100%).
- **Candle flicker too harsh** — adjust `smoothing` / `depth` / snuff constants in `candle::`.
- **Candle too windy / too calm with people nearby** — tune `RSSI_DELTA_FULL_DB`,
  `WIND_WEIGHT_BOOST_MAX`, `GUST_SPEED_MULT_MAX`, or `SNUFF_RATE_MULT_MAX` in `candle::`
  (subtle RSSI wind on default Candle).
- **Wifi RSSI preset too dim/bright for your AP** — adjust `RSSI_WEAK_DB` /
  `RSSI_STRONG_DB` / `WHITE_PEAK` / `RED_ONSET` / `GREEN_ONSET` in `wifi_rssi::`.
- **Red ember too strong** — adjust `RED_MIN` / `RED_MAX` / `RED_CURVE`.
- **Effect not applying to a channel** — check `id(pwm_led_N)` in the effect header; only effects / Preset / numbers should call `set_level` (no competing light wrappers).

## 5. OTA update test

Once adopted, an OTA update over Wi-Fi should work:

```bash
.venv/bin/esphome run esphome/espcandle.factory.factory.yaml --device espcandle-XXXXXX.local
```

(substitute the device's mDNS name; run from repo root). Uses the base's `ota: - platform: esphome` (no password — open on the trusted LAN; see `docs/adoption-strategy.md`).

## 6. What's not (yet) automated

- **Hardware-in-the-loop**: no automated rig to flash, power-cycle, and assert serial markers or LED output. CI runs config+compile only.
- **Effect visual regression**: there's no way to assert LED output without a camera or photodiode rig. Effect validation is manual (section 4).
- **First-boot / heap issues under load**: rare; needs serial capture on real hardware.

## CI

GitHub Actions workflow: `.github/workflows/ci.yml` (push / PR).

| Job | What it runs |
|---|---|
| `esphome-smoke` | `make smoke-test` (Python 3.13 + `esphome/requirements.txt`) |
| `pio-demo` | `pio run -e esp32s3` (standalone demo firmware) |

Local equivalent before opening a PR:

```bash
make smoke-test
pio run -e esp32s3
```

On-device / HA checks remain manual (sections 2–5).
