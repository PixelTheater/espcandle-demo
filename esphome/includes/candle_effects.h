// ESP Candle - custom lighting effects for ESPHome.
//
// Ports the standalone firmware's candle/color/magic engine (see src/main.cpp)
// into ESPHome addressable_lambda effects on the RGB strip light. The engine
// drives `it[]` (RGB only) and the four PWM channels (warm white 1/2, UV,
// deep red) via ledc float-output IDs (`id(pwm_led_*).set_level(...)`).
//
// PWM is owned exclusively by these effects / the Preset select / channel
// number entities — not by a partition light (that model fights dual writers).
//
// Boot button short-press cycles the Preset select (not effects directly).
// Auto mode is intentionally not ported.
//
// Tunables are grouped at the top of each section, mirroring src/config.h.

#pragma once

#include "esphome.h"
#include <cmath>

namespace espcandle {

// ---------------------------------------------------------------------------
// Self-contained 1D gradient noise (replaces FastLED inoise8, which is not
// linked with esp32_rmt_led_strip). Lattice values are biased into ~64..192
// like inoise8, then sample_noise() expands to 0..255 — same mapping as
// standalone src/main.cpp. Hermite smoothstep avoids the linear-lerp look
// that read as "slow random fades".
// ---------------------------------------------------------------------------
inline uint32_t hash31(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
// One lattice corner in the inoise8-like mid band (~64..192).
inline uint8_t noise_lattice(uint32_t i) {
  return (uint8_t)(64 + ((hash31(i) >> 16) * 128) / 255);
}
inline uint8_t noise8(uint32_t t) {
  uint32_t i = t >> 8;          // integer cell
  uint32_t f = t & 0xff;        // frac 0..255
  uint8_t a = noise_lattice(i);
  uint8_t b = noise_lattice(i + 1);
  // Hermite smoothstep: 3u^2 - 2u^3
  float u = f / 255.0f;
  u = u * u * (3.0f - 2.0f * u);
  return (uint8_t)(a + (int16_t(b) - int16_t(a)) * u);
}
// Rescale mid-band noise to a fuller 0..255 swing (standalone sampleNoise).
inline uint8_t sample_noise(uint32_t t) {
  int raw = (int) noise8(t);
  raw = (raw - 64) * 255 / 128;
  if (raw < 0) raw = 0;
  if (raw > 255) raw = 255;
  return (uint8_t) raw;
}

// ---------------------------------------------------------------------------
// Output ownership
//
// Preset scenes (anything except Off/Custom) own all outputs each effect tick.
// Custom / manual: each channel is independent —
//   White/Red/UV  → number % × enable switch  (absolute PWM, NOT gated by RGB)
//   RGB strip     → ESP Candle light (on/off + color + brightness) when
//                   rgb_enable is on; black when rgb_enable is off.
// Moving a channel control exits any scene / Off into Custom.
// ---------------------------------------------------------------------------

inline float clampf(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Master from the RGB light — used by *scene* effects only so the HA brightness
// slider dims the whole scene. Custom PWM must not use this.
inline float master_level() {
  const auto &v = id(rgb_strip).current_values;
  return clampf(v.get_brightness() * v.get_state());
}

inline bool rgb_strip_enabled() { return id(rgb_enable).state; }

inline bool is_custom_mode() {
  return id(candle_preset).current_option() == "Custom";
}

// Effect RGB write: scene effects honour RGB Strip enable.
inline void set_effect_rgb(esphome::light::AddressableLight &it,
                           const esphome::Color &color) {
  if (rgb_strip_enabled())
    it.all() = color;
  else
    it.all() = esphome::Color(0, 0, 0);
}
inline void set_effect_rgb(esphome::light::AddressableLight &it,
                           const esphome::light::ESPHSVColor &color) {
  if (rgb_strip_enabled())
    it.all() = color;
  else
    it.all() = esphome::Color(0, 0, 0);
}

inline int rnd(int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int) (esphome::random_uint32() % (uint32_t) (hi - lo));
}

inline void write_pwm(float white, float red, float uv, float master) {
  float m = clampf(master);
  id(pwm_led_1).set_level(clampf(white * m));
  id(pwm_led_2).set_level(clampf(white * m));
  id(pwm_led_4).set_level(clampf(red * m));
  id(pwm_led_3).set_level(clampf(uv * m));
}

// Scene effects: mix numbers × RGB-light master brightness.
inline void set_pwm_mastered(float white, float red, float uv) {
  write_pwm(white, red, uv, master_level());
}

inline void pwm_from_numbers() {
  set_pwm_mastered(id(white_level).state / 100.0f, id(red_level).state / 100.0f,
                   id(uv_level).state / 100.0f);
}

inline bool is_scene_preset(const std::string &preset) {
  return !preset.empty() && preset != "Off" && preset != "Custom";
}

// Addressable effects are 1-indexed; 0 means None (Off / Custom / not started).
inline bool scene_effect_active() {
  return id(rgb_strip).get_current_effect_index() != 0;
}

// Custom PWM: absolute levels × per-channel enables. Independent of RGB light.
inline void apply_custom_pwm(float white_pct, float red_pct, float uv_pct) {
  float w = id(white_enable).state ? white_pct / 100.0f : 0.0f;
  float r = id(red_enable).state ? red_pct / 100.0f : 0.0f;
  float u = id(uv_enable).state ? uv_pct / 100.0f : 0.0f;
  write_pwm(w, r, u, 1.0f);
}

inline void apply_custom_pwm() {
  apply_custom_pwm(id(white_level).state, id(red_level).state, id(uv_level).state);
}

// Stop a running scene effect and darken the strip. Scenes blank entity colour
// and paint only via the effect buffer — do not invent a white fill (that made
// Magic → toggle Red jump to solid white at scene brightness). rgb_enable goes
// false so the UI matches; user turns RGB Strip on when they want Custom colour.
//
// No-op when no scene effect is running (Off → Custom, RGB Strip on from Off).
// Killing RGB in that case turned the light back off on the first attempt.
inline void stop_scene_effect() {
  if (!scene_effect_active())
    return;
  const bool held = id(applying_preset);
  id(applying_preset) = true;
  {
    auto stop = id(rgb_strip).make_call();
    stop.set_effect("None");
    stop.perform();
  }
  id(rgb_enable).publish_state(false);
  id(rgb_strip).turn_off().perform();
  if (!held)
    id(applying_preset) = false;
}

// RGB Strip switch off (rgb_enable already published false by caller).
// Scenes: leave the effect running — set_effect_rgb writes black each tick.
// Custom/Off: turn the light off so WS2812s go dark but colour is preserved
// for the next turn-on (never set_rgb(0,0,0) — that kills the picker).
inline void rgb_strip_output_off() {
  auto preset = std::string(id(candle_preset).current_option().c_str());
  if (is_scene_preset(preset))
    return;
  const bool held = id(applying_preset);
  id(applying_preset) = true;
  id(rgb_strip).turn_off().perform();
  if (!held)
    id(applying_preset) = false;
}

// RGB Strip switch on (rgb_enable already published true by caller).
// Scenes: effect already running — next tick paints RGB again.
// Custom: turn the light on with effect None; keep existing colour.
// Off: same light turn-on, then Off→Custom (on_turn_on cannot do this while
// we hold applying_preset to avoid re-entry).
inline void rgb_strip_output_on() {
  auto preset = std::string(id(candle_preset).current_option().c_str());
  if (is_scene_preset(preset))
    return;
  const bool held = id(applying_preset);
  id(applying_preset) = true;
  {
    auto c = id(rgb_strip).turn_on();
    c.set_effect("None");
    const auto &v = id(rgb_strip).current_values;
    if (v.get_red() + v.get_green() + v.get_blue() < 0.01f)
      c.set_rgb(1.0f, 1.0f, 1.0f);
    if (v.get_brightness() < 0.01f)
      c.set_brightness(0.5f);
    c.perform();
  }
  if (!held)
    id(applying_preset) = false;

  // Mirror light on_turn_on: powering the strip from Off enters Custom.
  if (preset == "Off") {
    auto call = id(candle_preset).make_call();
    call.set_option("Custom");
    call.perform();
  }
}

// Exit any scene into Custom. Custom set_action stops the effect and applies
// PWM. No-op if already Custom (do not re-stop / flash the strip).
inline void ensure_custom_preset() {
  if (is_custom_mode())
    return;
  auto call = id(candle_preset).make_call();
  call.set_option("Custom");
  call.perform();
}

// Device-page channel slider. `x` for the moved channel is already substituted.
// Any user change leaves the scene so channels are independently controllable.
inline void on_channel_slider(float white_pct, float red_pct, float uv_pct,
                              char which) {
  if (id(applying_preset))
    return;

  // Auto-enable the channel when its level goes above zero.
  if (which == 'w' && white_pct > 0.5f)
    id(white_enable).publish_state(true);
  if (which == 'r' && red_pct > 0.5f)
    id(red_enable).publish_state(true);
  if (which == 'u' && uv_pct > 0.5f)
    id(uv_enable).publish_state(true);

  ensure_custom_preset();
  // Use explicit percents — template number .state is still the old value
  // until set_action returns.
  apply_custom_pwm(white_pct, red_pct, uv_pct);
}

inline void on_channel_enable_changed() {
  if (id(applying_preset))
    return;
  auto preset = std::string(id(candle_preset).current_option().c_str());
  if (is_scene_preset(preset)) {
    // Leave the scene so channels are independently controllable.
    ensure_custom_preset();
  } else if (preset == "Off" || preset.empty()) {
    // User turning a channel on from Off → Custom. All-off (boot ALWAYS_OFF
    // re-fires turn_off) must stay Off, or every reboot would land in Custom.
    if (id(white_enable).state || id(red_enable).state || id(uv_enable).state)
      ensure_custom_preset();
  }
  apply_custom_pwm();
}

// ===========================================================================
// NATURAL - static warm scene. Whites/red/UV from channel numbers × master;
// RGB is a fixed orange-white tint (color-corrected by master). At default
// entry (white=100%, master=50%) whites read ~50% and the strip ~30%.
// ===========================================================================
namespace natural {
  // V=153 ≈ 0.6; color correction × master 0.5 → ~30% perceived strip level.
  static constexpr uint8_t RGB_H = 28;   // warm amber / candle-orange
  static constexpr uint8_t RGB_S = 90;   // soft, not neon
  static constexpr uint8_t RGB_V = 153;

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    (void) initial;
    pwm_from_numbers();
    set_effect_rgb(it, esphome::light::ESPHSVColor(RGB_H, RGB_S, RGB_V));
  }
}  // namespace natural

// ===========================================================================
// WARM GLOW - static PWM ember (dim whites + red). RGB off. Tracks master.
// Default mix 10% / 40% at master 60% (scene entry values).
// ===========================================================================
namespace warm_glow {
  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    (void) initial;
    pwm_from_numbers();
    it.all() = esphome::Color(0, 0, 0);
  }
}  // namespace warm_glow

// ===========================================================================
// UV ACCENT - static UV wash + faint purple RGB tint. Tracks master.
// Default mix UV=50% at master 40%. Any channel change leaves for Custom.
// ===========================================================================
namespace uv_accent {
  static constexpr uint8_t RGB_H = 192;
  static constexpr uint8_t RGB_S = 220;
  static constexpr uint8_t RGB_V = 40;

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    (void) initial;
    pwm_from_numbers();
    set_effect_rgb(it, esphome::light::ESPHSVColor(RGB_H, RGB_S, RGB_V));
  }
}  // namespace uv_accent

// ===========================================================================
// CANDLE - Perlin-noise flame simulation across the warm whites + red ember.
//
// Three sub-modes (CALM / FLICKER / WIND) cycle on weighted-random schedules
// and crossfade. Two warm-white channels drift on independent noise axes so
// they never lock together. A red ember glows inverse to flame brightness.
// The RGB strip is held off (this is the warm-flame mode). UV is off.
//
// Live WiFi RSSI deltas bias an "air" factor (0..1): unsettled RF raises
// WIND pick weight, gust-envelope speed, and snuff rate. Absolute RSSI is
// ignored so a far bedroom stays calm. air=0 keeps the base schedule alone.
//
// Drives: white_1, white_2, red. RGB strip black. UV off.
// ===========================================================================
namespace candle {
  // Sub-mode time allocation (% must sum to 100).
  static constexpr int   PCT_CALM    = 45;
  static constexpr int   PCT_FLICKER = 45;
  static constexpr int   PCT_WIND    = 10;
  static constexpr unsigned long SUBMODE_MIN_MS = 4000;
  static constexpr unsigned long SUBMODE_MAX_MS = 20000;
  static constexpr unsigned long XFADE_MS        = 600;

  // Red ember: fraction of BRIGHTNESS_MAX_RED at peak / near-zero flame.
  static constexpr float RED_MIN   = 0.10f;
  static constexpr float RED_MAX   = 0.35f;
  static constexpr float RED_CURVE = 1.2f;

  // Per-channel white drift.
  static constexpr float W1_SPEED = 0.022f;
  static constexpr float W2_SPEED = 0.037f;
  static constexpr float SPLIT_DEPTH_CALM    = 0.50f;
  static constexpr float SPLIT_DEPTH_FLICKER = 0.25f;

  // Snuff events (flicker only): brief soft dip — not a hard black flash.
  // Rate is time-based (~1 attempt/sec/channel). Trigger and recover are
  // separate so a single tick does not erase the dip.
  static constexpr float SNUFF_PER_SEC = 1.0f;
  static constexpr float SNUFF_DEPTH   = 0.45f;  // dip to ~55% (was 3% — too glitchy)
  static constexpr float SNUFF_RECOVER = 0.035f; // slower recover = softer edge

  // RSSI → air (subtle wind). Samples wifi::wifi_rssi() directly so the
  // effect is not limited to the 60 s HA wifi_signal diagnostic interval.
  static constexpr unsigned long RSSI_SAMPLE_MS = 1500;
  static constexpr float RSSI_DELTA_FULL_DB    = 8.0f;   // |ΔdBm| that saturates air→1
  static constexpr float AIR_RISE_ALPHA        = 0.45f;  // attack on RF jumps
  static constexpr float AIR_FALL_ALPHA        = 0.06f;  // slow release
  static constexpr int   WIND_WEIGHT_BOOST_MAX = 15;     // PCT_WIND 10 → up to 25
  static constexpr float GUST_SPEED_MULT_MAX   = 1.4f;
  static constexpr float SNUFF_RATE_MULT_MAX   = 1.25f;

  // white_max_power on the ledc outputs (must match espcandle.base.yaml).
  // Standalone writes absolute fractions of MAX_DUTY; ESPHome set_level is
  // relative to max_power, so divide by WHITE_MAX to restore the same watts.
  static constexpr float WHITE_MAX = 0.40f;

  // Per-sub-mode brightness/depth/speed/smoothing.
  // Speeds match standalone (tempo); smoothing rates are lower so steep
  // noise edges lag instead of flashing. Depths slightly softer than
  // standalone so the floor is not near-black.
  // Brightness peaks match src/config.h (0.07 / 0.11 / 0.13 of full duty).
  struct SubParams {
    float brightness, depth, speed, speed2, octave2, smoothing;
  };
  static constexpr SubParams CALM    { 0.07f / WHITE_MAX, 0.50f, 0.018f, 0.07f, 0.30f, 0.030f };
  static constexpr SubParams FLICKER { 0.11f / WHITE_MAX, 0.55f, 0.14f,  0.60f, 0.22f, 0.045f };
  static constexpr SubParams WIND    { 0.13f / WHITE_MAX, 0.72f, 0.28f,  1.20f, 0.35f, 0.065f };
  static constexpr float WIND_GUST_SPEED = 0.006f;
  static constexpr float WIND_GUST_DEPTH  = 0.70f;

  enum class Sub : uint8_t { CALM, FLICKER, WIND };

  struct State {
    Sub sub = Sub::FLICKER;
    Sub next_sub = Sub::FLICKER;
    unsigned long sub_end = 0;
    unsigned long xfade_end = 0;
    float t_fast = 0, t_fast2 = 0, t_gust = 0, t_w1 = 0, t_w2 = 0;
    float snuff_w1 = 1.0f, snuff_w2 = 1.0f;
    // Initial levels as set_level (absolute 0.08 / WHITE_MAX), matching enterCandleMode.
    float out_w1 = 0.08f / WHITE_MAX, out_w2 = 0.08f / WHITE_MAX, out_red = 0.05f;
    unsigned long last = 0;
    // RSSI wind: air in [0,1]; last_rssi only valid when rssi_have.
    float air = 0.0f;
    int8_t last_rssi = 0;
    bool rssi_have = false;
    unsigned long rssi_next = 0;
  };
  static State s;

  // Sample live RSSI and update s.air. Decays toward 0 when WiFi is down.
  static void update_air(unsigned long now) {
    if (now < s.rssi_next) return;
    s.rssi_next = now + RSSI_SAMPLE_MS;

    float target = 0.0f;
#ifdef USE_WIFI
    if (esphome::wifi::global_wifi_component != nullptr) {
      int8_t rssi = esphome::wifi::global_wifi_component->wifi_rssi();
      if (rssi != esphome::wifi::WIFI_RSSI_DISCONNECTED) {
        if (!s.rssi_have) {
          s.last_rssi = rssi;
          s.rssi_have = true;
          return;  // need a second sample before any delta
        }
        int d = (int) rssi - (int) s.last_rssi;
        if (d < 0) d = -d;
        s.last_rssi = rssi;
        target = clampf((float) d / RSSI_DELTA_FULL_DB);
      } else {
        s.rssi_have = false;
      }
    }
#else
    (void) now;
#endif
    float alpha = (target > s.air) ? AIR_RISE_ALPHA : AIR_FALL_ALPHA;
    s.air += (target - s.air) * alpha;
    if (s.air < 0.001f) s.air = 0.0f;
  }

  static Sub pick_next(Sub cur) {
    int wind_w = PCT_WIND + (int)(s.air * (float) WIND_WEIGHT_BOOST_MAX + 0.5f);
    struct { Sub m; int w; } t[3] = {
      {Sub::CALM, PCT_CALM},
      {Sub::FLICKER, PCT_FLICKER},
      {Sub::WIND, wind_w},
    };
    int total = 0;
    for (auto& e : t) if (e.m != cur) total += e.w;
    if (total == 0) return cur;
    int r = rnd(0, total), acc = 0;
    for (auto& e : t) { if (e.m == cur) continue; acc += e.w; if (r < acc) return e.m; }
    return Sub::FLICKER;
  }
  static const SubParams& params(Sub m) {
    switch (m) { case Sub::CALM: return CALM; case Sub::FLICKER: return FLICKER; default: return WIND; }
  }

  static float level(Sub m, uint8_t n1, uint8_t n2, uint8_t gust) {
    const SubParams& p = params(m);
    float n = (n1 * (1.0f - p.octave2) + n2 * p.octave2) / 255.0f;
    float base = p.brightness * (1.0f - p.depth);
    float lvl = base + p.brightness * p.depth * n;
    if (m == Sub::WIND) {
      float g = gust / 255.0f;
      lvl *= 1.0f - WIND_GUST_DEPTH * (1.0f - g);
    }
    return lvl;
  }

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    if (initial) {
      it.all() = esphome::Color(0, 0, 0);
      s.sub = Sub::FLICKER;
      s.air = 0.0f;
      s.rssi_have = false;
      s.rssi_next = 0;
      s.next_sub = pick_next(s.sub);
      s.sub_end = millis() + rnd(SUBMODE_MIN_MS, SUBMODE_MAX_MS);
      s.xfade_end = 0;
      s.t_fast = rnd(0, 10000); s.t_fast2 = rnd(10000, 30000);
      s.t_gust = rnd(0, 10000);
      s.t_w1 = rnd(0, 10000); s.t_w2 = rnd(20000, 40000);
      s.snuff_w1 = s.snuff_w2 = 1.0f;
      s.out_w1 = s.out_w2 = 0.08f / WHITE_MAX;
      s.out_red = 0.05f;
      s.last = millis();
    }
    unsigned long now = millis();
    unsigned long dt = now - s.last;
    if (dt == 0) return;
    s.last = now;
    float dtf = (float) dt;

    update_air(now);

    // Sub-mode scheduler with crossfade.
    if (now >= s.sub_end && s.xfade_end == 0) {
      s.next_sub = pick_next(s.sub);
      s.xfade_end = now + XFADE_MS;
      s.sub_end = s.xfade_end + rnd(SUBMODE_MIN_MS, SUBMODE_MAX_MS);
    }
    if (s.xfade_end != 0 && now >= s.xfade_end) { s.sub = s.next_sub; s.xfade_end = 0; }

    // Advance noise axes. Gust envelope speeds up when RF air is unsettled.
    const SubParams& p = params(s.sub);
    float gust_mult = 1.0f + s.air * (GUST_SPEED_MULT_MAX - 1.0f);
    s.t_fast  += p.speed  * dtf;
    s.t_fast2 += p.speed2 * dtf;
    s.t_gust  += WIND_GUST_SPEED * gust_mult * dtf;
    s.t_w1    += W1_SPEED * dtf;
    s.t_w2    += W2_SPEED * dtf;

    uint8_t n1   = sample_noise((uint32_t) s.t_fast);
    uint8_t n2   = sample_noise((uint32_t) s.t_fast2);
    uint8_t gust = sample_noise((uint32_t) s.t_gust);
    float cur_level = level(s.sub, n1, n2, gust);

    // Crossfade toward next sub-mode.
    if (s.xfade_end != 0) {
      float t = 1.0f - (float)(s.xfade_end - now) / (float) XFADE_MS;
      t = clampf(t);
      float next_level = level(s.next_sub,
        sample_noise((uint32_t)(s.t_fast + 7919)),
        sample_noise((uint32_t)(s.t_fast2 + 5003)), gust);
      cur_level = cur_level + (next_level - cur_level) * t;
    }

    // W1/W2 independent drift around cur_level.
    float split = (s.sub == Sub::CALM) ? SPLIT_DEPTH_CALM : SPLIT_DEPTH_FLICKER;
    float w1n = sample_noise((uint32_t) s.t_w1) / 255.0f;
    float w2n = sample_noise((uint32_t) s.t_w2) / 255.0f;
    float target_w1 = clampf(cur_level * (1.0f + split * (w1n - 0.5f) * 2.0f));
    float target_w2 = clampf(cur_level * (1.0f + split * (w2n - 0.5f) * 2.0f));

    // Snuff events (flicker only) — time-based trigger; recover on later ticks.
    float snuff_rate = SNUFF_PER_SEC * (1.0f + s.air * (SNUFF_RATE_MULT_MAX - 1.0f));
    auto snuff_tick = [&](float &snuff) {
      if (snuff >= 1.0f) {
        int threshold = (int)(snuff_rate * 10.0f * dtf);
        if (threshold > 0 && rnd(0, 10000) < threshold)
          snuff = 1.0f - SNUFF_DEPTH;
      } else {
        float a = 1.0f - expf(-SNUFF_RECOVER * dtf);
        snuff += (1.0f - snuff) * clampf(a);
        if (snuff > 0.999f) snuff = 1.0f;
      }
    };
    if (s.sub == Sub::FLICKER) {
      snuff_tick(s.snuff_w1);
      snuff_tick(s.snuff_w2);
    } else {
      s.snuff_w1 = s.snuff_w2 = 1.0f;
    }
    target_w1 *= s.snuff_w1;
    target_w2 *= s.snuff_w2;

    // Red ember: inverse curve over its own cap; max_power (80%) applies the rest.
    float peak = params(s.sub).brightness;
    float dimness = 1.0f - clampf(cur_level / peak);
    float red_of_cap = RED_MIN + (RED_MAX - RED_MIN) * powf(dimness, RED_CURVE);
    float target_red = red_of_cap;

    // Continuous-time exponential smoothing — lower rates keep tempo but
    // round off steep high↔low edges so flicker does not look glitchy.
    float alpha = 1.0f - expf(-p.smoothing * dtf);
    s.out_w1  += (target_w1  - s.out_w1)  * alpha;
    s.out_w2  += (target_w2  - s.out_w2)  * alpha;
    s.out_red += (target_red - s.out_red) * alpha;

    // Drive channels, scaled by the light's HA brightness slider.
    float master = master_level();
    id(pwm_led_1).set_level(clampf(s.out_w1 * master));
    id(pwm_led_2).set_level(clampf(s.out_w2 * master));
    id(pwm_led_4).set_level(clampf(s.out_red * master));
    id(pwm_led_3).set_level(0.0f);  // UV off in candle mode

    // RGB strip off (correction still applies master to blacks — fine).
    it.all() = esphome::Color(0, 0, 0);
  }
}  // namespace candle

// ===========================================================================
// WIFI RSSI - steady diagnostic gauge (no flicker).
//
// Absolute WiFi signal → white level. Red fades in when the link is weak.
// RGB strip starts glowing green as the signal gets strong. UV off.
// ===========================================================================
namespace wifi_rssi {
  static constexpr float WHITE_MAX = 0.40f;

  // Absolute RSSI → 0..1 signal. Typical STA range ~-90 … -40 dBm.
  static constexpr unsigned long SAMPLE_MS = 1000;
  static constexpr float RSSI_WEAK_DB     = -88.0f;
  static constexpr float RSSI_STRONG_DB   = -42.0f;
  static constexpr float SIGNAL_EMA       = 0.18f;  // smooth dBm noise
  static constexpr float OUT_SMOOTH       = 0.08f;  // output lerp per ms

  // White: steady level, no Perlin. Peak as fraction of full duty → set_level.
  static constexpr float WHITE_PEAK = 0.22f / WHITE_MAX;  // readable at strong RSSI
  static constexpr float WHITE_FLOOR = 0.02f / WHITE_MAX; // barely-on when dead

  // Red: kicks in below RED_ONSET (signal), peaks at RED_PEAK when signal≈0.
  static constexpr float RED_ONSET = 0.40f;
  static constexpr float RED_PEAK  = 0.55f;  // of red max_power
  static constexpr float RED_CURVE = 1.4f;

  // Green RGB: starts above GREEN_ONSET, full V at strong signal.
  static constexpr float GREEN_ONSET = 0.55f;
  static constexpr uint8_t GREEN_H = 96;   // pure-ish green on HSV wheel
  static constexpr uint8_t GREEN_S = 220;
  static constexpr uint8_t GREEN_V_MAX = 140;

  struct State {
    float signal = 0.0f;
    float out_w = 0.0f, out_red = 0.0f, out_g = 0.0f;
    unsigned long last = 0;
    unsigned long next_sample = 0;
    bool have_rssi = false;
  };
  static State s;

  static float rssi_to_signal(int8_t rssi) {
#ifdef USE_WIFI
    if (rssi == esphome::wifi::WIFI_RSSI_DISCONNECTED)
      return 0.0f;
#endif
    return clampf((float(rssi) - RSSI_WEAK_DB) / (RSSI_STRONG_DB - RSSI_WEAK_DB));
  }

  static void sample(unsigned long now) {
    if (now < s.next_sample) return;
    s.next_sample = now + SAMPLE_MS;

    float target = 0.0f;
#ifdef USE_WIFI
    if (esphome::wifi::global_wifi_component != nullptr) {
      int8_t rssi = esphome::wifi::global_wifi_component->wifi_rssi();
      target = rssi_to_signal(rssi);
      if (!s.have_rssi) {
        s.signal = target;
        s.have_rssi = true;
        return;
      }
    }
#else
    (void) now;
#endif
    s.signal += (target - s.signal) * SIGNAL_EMA;
  }

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    if (initial) {
      s.signal = 0.0f;
      s.out_w = s.out_red = s.out_g = 0.0f;
      s.have_rssi = false;
      s.next_sample = 0;
      s.last = millis();
      it.all() = esphome::Color(0, 0, 0);
    }
    unsigned long now = millis();
    unsigned long dt = now - s.last;
    if (dt == 0) return;
    s.last = now;
    float dtf = (float) dt;

    sample(now);

    // White tracks signal steadily (no flicker).
    float target_w = WHITE_FLOOR + (WHITE_PEAK - WHITE_FLOOR) * s.signal;

    // Red only when weak: full at 0, off by RED_ONSET.
    float red_t = 0.0f;
    if (s.signal < RED_ONSET)
      red_t = powf((RED_ONSET - s.signal) / RED_ONSET, RED_CURVE);
    float target_red = RED_PEAK * red_t;

    // Green RGB above GREEN_ONSET.
    float green_t = 0.0f;
    if (s.signal > GREEN_ONSET)
      green_t = clampf((s.signal - GREEN_ONSET) / (1.0f - GREEN_ONSET));
    float target_g = green_t;

    float a = clampf(OUT_SMOOTH * dtf);
    s.out_w   += (target_w   - s.out_w)   * a;
    s.out_red += (target_red - s.out_red) * a;
    s.out_g   += (target_g   - s.out_g)   * a;

    float master = master_level();
    id(pwm_led_1).set_level(clampf(s.out_w * master));
    id(pwm_led_2).set_level(clampf(s.out_w * master));
    id(pwm_led_4).set_level(clampf(s.out_red * master));
    id(pwm_led_3).set_level(0.0f);

    uint8_t gv = (uint8_t)(GREEN_V_MAX * clampf(s.out_g) + 0.5f);
    if (gv > 0)
      set_effect_rgb(it, esphome::light::ESPHSVColor(GREEN_H, GREEN_S, gv));
    else
      it.all() = esphome::Color(0, 0, 0);
  }
}  // namespace wifi_rssi

// ===========================================================================
// EMBER - slow deep-red glow with a quiet white breath. RGB strip off.
// ===========================================================================
namespace ember {
  // Levels match HA number percents (white=2, red=10); soft sway around them.
  static constexpr float RED_BASE    = 0.10f;
  static constexpr float RED_DEPTH   = 0.035f;
  static constexpr float WHITE_BASE  = 0.02f;
  static constexpr float WHITE_DEPTH = 0.008f;
  static constexpr float NOISE_SPEED = 0.0045f;
  static constexpr float SMOOTH      = 0.04f;

  struct State {
    float t = 0, t2 = 0;
    float out_red = 0.10f, out_w = 0.02f;
    unsigned long last = 0;
  };
  static State s;

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    if (initial) {
      it.all() = esphome::Color(0, 0, 0);
      s.t = (float) rnd(0, 10000);
      s.t2 = (float) rnd(20000, 40000);
      s.out_red = RED_BASE;
      s.out_w = WHITE_BASE;
      s.last = millis();
    }
    unsigned long now = millis();
    unsigned long dt = now - s.last;
    if (dt == 0) return;
    s.last = now;
    float dtf = (float) dt;

    s.t  += NOISE_SPEED * dtf;
    s.t2 += NOISE_SPEED * 0.63f * dtf;
    float n1 = sample_noise((uint32_t) s.t) / 255.0f;
    float n2 = sample_noise((uint32_t) s.t2) / 255.0f;
    float target_red = RED_BASE + RED_DEPTH * (n1 - 0.5f) * 2.0f;
    float target_w   = WHITE_BASE + WHITE_DEPTH * (n2 - 0.5f) * 2.0f;
    if (target_red < 0.04f) target_red = 0.04f;
    if (target_w < 0.0f) target_w = 0.0f;

    float alpha = clampf(SMOOTH * dtf);
    s.out_red += (target_red - s.out_red) * alpha;
    s.out_w   += (target_w   - s.out_w)   * alpha;

    float master = master_level();
    id(pwm_led_1).set_level(clampf(s.out_w * master));
    id(pwm_led_2).set_level(clampf(s.out_w * 0.85f * master));
    id(pwm_led_4).set_level(clampf(s.out_red * master));
    id(pwm_led_3).set_level(0.0f);
    it.all() = esphome::Color(0, 0, 0);
  }
}  // namespace ember

// ===========================================================================
// COLOR - slow HSV drift with a speed random-walk and a spatial history
// buffer for a spread of hues along the strip. Spatial blur softens edges.
//
// Drives: RGB strip. White/UV/red off.
// ===========================================================================
namespace color {
  // ~1/4 of the original port speeds; direction can reverse.
  static constexpr float SPEED_MIN = 0.0005f;
  static constexpr float SPEED_MAX = 0.030f;
  // Write at full V so the light entity's brightness slider is the master
  // (ESPHome color-corrects addressable pixels by brightness × state).
  static constexpr int   SATURATION = 240;
  static constexpr int   BRIGHTNESS = 255;
  // Stretch hues along the strip (look further back in history per LED).
  static constexpr int   SPATIAL_STRETCH = 3;
  static constexpr unsigned long REVERSE_MIN_MS = 12000;
  static constexpr unsigned long REVERSE_MAX_MS = 45000;

  struct State {
    float hue = 0;
    float speed = 0.004f;   // signed: negative = reverse
    float vel = 0;
    unsigned long last = 0;
    unsigned long reverse_at = 0;
  };
  static State s;
  static constexpr int HIST = 20 * SPATIAL_STRETCH + 8;
  static uint8_t hist[HIST];
  static int hist_idx = 0;
  static unsigned long last_push = 0;

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    if (initial) {
      it.all() = esphome::Color(0, 0, 0);
      s.hue = rnd(0, 256);
      s.speed = SPEED_MIN + (SPEED_MAX - SPEED_MIN) * 0.35f;
      if (rnd(0, 2) == 0) s.speed = -s.speed;
      s.vel = 0;
      s.last = millis();
      last_push = millis();
      hist_idx = 0;
      s.reverse_at = millis() + rnd(REVERSE_MIN_MS, REVERSE_MAX_MS);
      for (int i = 0; i < HIST; i++) hist[i] = (uint8_t) s.hue;
    }
    unsigned long now = millis();
    unsigned long dt = now - s.last;
    if (dt < 20) return;
    s.last = now;
    float dtf = (float) dt;

    // Occasional reverse.
    if (now >= s.reverse_at) {
      s.speed = -s.speed;
      s.vel = 0;
      s.reverse_at = now + rnd(REVERSE_MIN_MS, REVERSE_MAX_MS);
    }

    // Gentle speed random walk (smoother than the original port).
    float abs_centre = (SPEED_MIN + SPEED_MAX) * 0.5f;
    float signed_centre = (s.speed >= 0.0f) ? abs_centre : -abs_centre;
    s.vel += (((float) rnd(0, 1000) / 500.0f) - 1.0f) * 0.0000006f * dtf;
    s.vel *= 0.985f;
    s.vel += (signed_centre - s.speed) * 0.000002f * dtf;
    s.speed += s.vel * dtf;
    float as = fabsf(s.speed);
    if (as < SPEED_MIN) s.speed = (s.speed < 0.0f) ? -SPEED_MIN : SPEED_MIN;
    if (as > SPEED_MAX) s.speed = (s.speed < 0.0f) ? -SPEED_MAX : SPEED_MAX;

    s.hue += s.speed * dtf;
    while (s.hue >= 256.0f) s.hue -= 256.0f;
    while (s.hue < 0.0f) s.hue += 256.0f;

    // Push history slowly so the strip holds a long stretched spectrum.
    float speed_n = fabsf(s.speed) / 0.004f;
    if (speed_n < 0.25f) speed_n = 0.25f;
    unsigned long interval = (unsigned long)(480.0f / speed_n);
    if (interval < 80) interval = 80;
    if (interval > 3500) interval = 3500;
    if (now - last_push > interval) {
      hist[hist_idx] = (uint8_t) s.hue;
      hist_idx = (hist_idx + 1) % HIST;
      last_push = now;
    }

    int n = it.size();
    for (int i = 0; i < n; i++) {
      int look = i * SPATIAL_STRETCH;
      int pos = (hist_idx - look + HIST * 8) % HIST;
      it[i] = esphome::light::ESPHSVColor(hist[pos], SATURATION, BRIGHTNESS);
    }
    // Two-pass spatial blur for smoother transitions.
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < n; i++) {
        int prev = (i - 1 + n) % n;
        int next = (i + 1) % n;
        esphome::Color c = it[i].get() * 160;
        esphome::Color p = it[prev].get() * 48;
        esphome::Color nx = it[next].get() * 48;
        it[i] = c + p + nx;
      }
    }
    if (!rgb_strip_enabled())
      it.all() = esphome::Color(0, 0, 0);

    id(pwm_led_1).set_level(0.0f);
    id(pwm_led_2).set_level(0.0f);
    id(pwm_led_3).set_level(0.0f);
    id(pwm_led_4).set_level(0.0f);
  }
}  // namespace color

// ===========================================================================
// MAGIC - per-LED independent purple/blue drift, with unpredictable spark
// bursts (variable length, density, and fade). UV + red on; whites off.
// ===========================================================================
namespace magic {
  static constexpr float HUE_CENTER = 175.0f;
  static constexpr float HUE_SPREAD = 55.0f;
  static constexpr unsigned long DRIFT_MIN = 3000, DRIFT_MAX = 28000;
  static constexpr unsigned long SPARK_MIN  = 400, SPARK_MAX  = 14000;
  static constexpr float LED_SPEED_MIN = 0.002f, LED_SPEED_MAX = 0.028f;
  // Drift V is intentionally low vs sparks; light brightness scales both via
  // color correction. Keep headroom so the HA slider still has useful range.
  static constexpr int   DRIFT_BRIGHTNESS = 55;
  static constexpr int   DRIFT_RIPPLE     = 22;
  static constexpr int   SPARK_BRI_MIN    = 200, SPARK_BRI_MAX = 255;
  static constexpr int   SPARK_HUE_SPREAD = 36;
  static constexpr float RED_STEP = 0.000006f, RED_DAMP = 0.985f;
  static constexpr float RED_CENTRE = 0.45f, RED_PULL = 0.000008f;
  static constexpr float RED_MIN = 0.15f, RED_MAX = 0.85f;
  static constexpr int   MAX_LEDS = 24;

  enum class Phase { DRIFT, SPARK };
  struct LedState {
    float hue = 170.0f;
    float speed = 0.01f;
    float phase = 0.0f;
    uint8_t spark_v = 0;
    uint8_t spark_fade = 200;
    uint8_t spark_hue = 175;
  };
  struct State {
    Phase phase = Phase::DRIFT;
    unsigned long phase_end = 0;
    int spark_chance = 12;       // % per tick during SPARK (re-rolled each phase)
    int spark_burst = 1;         // how many sparks to try per tick
    float red_level = 0.4f, red_vel = 0;
    unsigned long last = 0;
    LedState led[MAX_LEDS];
  };
  static State s;

  static void reseed_leds(int n) {
    for (int i = 0; i < n && i < MAX_LEDS; i++) {
      s.led[i].hue = HUE_CENTER - HUE_SPREAD * 0.5f +
                     ((float) rnd(0, 1000) / 1000.0f) * HUE_SPREAD;
      s.led[i].speed = LED_SPEED_MIN +
                       ((float) rnd(0, 1000) / 1000.0f) * (LED_SPEED_MAX - LED_SPEED_MIN);
      if (rnd(0, 100) < 35) s.led[i].speed = -s.led[i].speed;
      s.led[i].phase = (float) rnd(0, 628) / 100.0f;
      s.led[i].spark_v = 0;
    }
  }

  static void start_phase(int n) {
    unsigned long now = millis();
    // Unpredictable: sometimes stay in the same kind of phase, or take a short blink.
    int roll = rnd(0, 100);
    if (s.phase == Phase::DRIFT) {
      if (roll < 78) {
        s.phase = Phase::SPARK;
        s.phase_end = now + rnd(SPARK_MIN, SPARK_MAX);
        s.spark_chance = rnd(4, 35);
        s.spark_burst = rnd(1, 5);
      } else {
        // Extra drift stretch instead of sparks.
        s.phase_end = now + rnd(DRIFT_MIN, DRIFT_MAX);
        reseed_leds(n);
      }
    } else {
      if (roll < 70) {
        s.phase = Phase::DRIFT;
        s.phase_end = now + rnd(DRIFT_MIN, DRIFT_MAX);
        reseed_leds(n);
      } else if (roll < 88) {
        // Another spark burst with fresh density.
        s.phase_end = now + rnd(SPARK_MIN, SPARK_MAX);
        s.spark_chance = rnd(6, 45);
        s.spark_burst = rnd(1, 6);
      } else {
        // Very short pop.
        s.phase_end = now + rnd(250, 900);
        s.spark_chance = rnd(25, 70);
        s.spark_burst = rnd(2, 7);
      }
    }
  }

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    int n = it.size();
    if (n > MAX_LEDS) n = MAX_LEDS;

    if (initial) {
      it.all() = esphome::Color(0, 0, 0);
      unsigned long now = millis();
      s.phase = Phase::DRIFT;
      s.phase_end = now + rnd(DRIFT_MIN, DRIFT_MAX);
      s.red_level = 0.4f; s.red_vel = 0;
      s.last = now;
      reseed_leds(n);
    }
    unsigned long now = millis();
    unsigned long dt = now - s.last;
    if (dt == 0) return;
    s.last = now;
    float dtf = (float) dt;

    if (now >= s.phase_end) start_phase(n);

    s.red_vel += (((float) rnd(0, 1000) / 500.0f) - 1.0f) * RED_STEP * dtf;
    s.red_vel *= RED_DAMP;
    s.red_vel += (RED_CENTRE - s.red_level) * RED_PULL * dtf;
    s.red_level += s.red_vel * dtf;
    if (s.red_level < RED_MIN) s.red_level = RED_MIN;
    if (s.red_level > RED_MAX) s.red_level = RED_MAX;
    float master = master_level();
    id(pwm_led_4).set_level(clampf(s.red_level * master));
    id(pwm_led_3).set_level(clampf(0.55f * master));

    if (s.phase == Phase::DRIFT) {
      for (int i = 0; i < n; i++) {
        auto &L = s.led[i];
        L.hue += L.speed * dtf;
        while (L.hue >= 256.0f) L.hue -= 256.0f;
        while (L.hue < 0.0f) L.hue += 256.0f;
        L.phase += 0.0015f * dtf * (0.6f + fabsf(L.speed) * 40.0f);
        uint8_t v = (uint8_t) clampf(
            (float) DRIFT_BRIGHTNESS +
            sinf(L.phase + (float) i * 0.7f) * (float) DRIFT_RIPPLE);
        it[i] = esphome::light::ESPHSVColor((uint8_t) L.hue, 230, v);
      }
    } else {
      // Decay existing sparks at per-LED rates; base is a dim tint of each LED's hue.
      for (int i = 0; i < n; i++) {
        auto &L = s.led[i];
        L.hue += L.speed * 0.35f * dtf;
        while (L.hue >= 256.0f) L.hue -= 256.0f;
        while (L.hue < 0.0f) L.hue += 256.0f;
        if (L.spark_v > 0) {
          int nv = (int) L.spark_v - (256 - (int) L.spark_fade);
          L.spark_v = (nv > 0) ? (uint8_t) nv : 0;
        }
        uint8_t tint = 12 + (uint8_t)(fabsf(sinf(L.phase)) * 10.0f);
        esphome::Color base =
            esphome::light::ESPHSVColor((uint8_t) L.hue, 220, tint).to_rgb();
        if (L.spark_v > 0) {
          esphome::Color spark =
              esphome::light::ESPHSVColor(L.spark_hue, 190, L.spark_v).to_rgb();
          it[i] = base + spark;
        } else {
          it[i] = base;
        }
      }
      // Spawn unpredictable sparks.
      if (rnd(0, 100) < s.spark_chance) {
        int count = s.spark_burst;
        if (rnd(0, 100) < 20) count += rnd(1, 4);
        for (int k = 0; k < count; k++) {
          int pos = rnd(0, n);
          auto &L = s.led[pos];
          L.spark_v = (uint8_t)(SPARK_BRI_MIN + rnd(0, SPARK_BRI_MAX - SPARK_BRI_MIN));
          L.spark_fade = (uint8_t) rnd(160, 250);  // higher = slower fade
          L.spark_hue = (uint8_t)(L.hue + rnd(0, SPARK_HUE_SPREAD * 2) - SPARK_HUE_SPREAD);
        }
      }
    }
    if (!rgb_strip_enabled())
      it.all() = esphome::Color(0, 0, 0);

    id(pwm_led_1).set_level(0.0f);
    id(pwm_led_2).set_level(0.0f);
  }
}  // namespace magic

}  // namespace espcandle
