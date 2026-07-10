// ESP Candle - custom lighting effects for ESPHome.
//
// Ports the standalone firmware's candle/color/magic engine (see src/main.cpp)
// into ESPHome addressable_lambda effects. The engine drives the addressable
// RGB strip via `it[]` and the four PWM channels (warm white 1/2, UV, deep red)
// directly via their ledc float-output IDs (`id(pwm_led_*).set_level(...)`).
//
// Why a header, not inline lambdas: the engine is ~400 lines of Perlin-noise
// flame simulation and state machines. Keeping it in C++ (not YAML blobs) makes
// it readable, tunable, and unit-checkable by the compiler.
//
// The three effects are wired from espcandle.base.yaml as `addressable_lambda`
// effects named Candle / Color / Magic. The boot button cycles them via
// light.effect.next. Auto mode (random effect cycling) is intentionally NOT
// ported: light.effect.next + the button already give the user the same UX,
// and an interval-based random cycler can be added later if desired.
//
// Tunables are grouped at the top of each section, mirroring src/config.h.

#pragma once

#include "esphome.h"

namespace espcandle {

// ---------------------------------------------------------------------------
// Self-contained 1D value noise (replaces FastLED inoise8, which is not linked
// when using esp32_rmt_led_strip). Returns 0..255. Cheap hash + linear interp;
// good enough for organic flame flicker.
// ---------------------------------------------------------------------------
inline uint32_t hash31(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
inline uint8_t noise8(uint32_t t) {
  uint32_t i = t >> 8;          // integer cell
  uint32_t f = t & 0xff;        // frac 0..255
  uint8_t a = (hash31(i) >> 16);
  uint8_t b = (hash31(i + 1) >> 16);
  return a + (int16_t(b) - int16_t(a)) * int16_t(f) / 255;
}
// Rescale from the ~64..192 cluster around 128 up to a fuller 0..255 swing,
// matching the standalone firmware's sampleNoise() mapping.
inline uint8_t sample_noise(uint32_t t) {
  int raw = (int) noise8(t);
  raw = (raw - 64) * 255 / 128;
  if (raw < 0) raw = 0;
  if (raw > 255) raw = 255;
  return (uint8_t) raw;
}

// Convenience: clamp to 0..1.
inline float clampf(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Random integer in [lo, hi). ESPHome under ESP-IDF exposes the no-arg C
// random(), not Arduino's random(min,max), so wrap esphome::random_uint32().
inline int rnd(int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int) (esphome::random_uint32() % (uint32_t) (hi - lo));
}

// ===========================================================================
// CANDLE - Perlin-noise flame simulation across the warm whites + red ember.
//
// Three sub-modes (CALM / FLICKER / WIND) cycle on weighted-random schedules
// and crossfade. Two warm-white channels drift on independent noise axes so
// they never lock together. A red ember glows inverse to flame brightness.
// The RGB strip is held off (this is the warm-flame mode). UV is off.
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

  // Snuff events (flicker only): one white channel briefly dips near zero.
  static constexpr int   SNUFF_CHANCE  = 2;     // per 1000 per tick
  static constexpr float SNUFF_DEPTH  = 0.97f;
  static constexpr float SNUFF_RECOVER = 0.08f;

  // Per-sub-mode brightness/depth/speed/smoothing.
  struct SubParams {
    float brightness, depth, speed, speed2, octave2, smoothing;
  };
  static constexpr SubParams CALM    { 0.07f, 0.60f, 0.018f, 0.07f, 0.35f, 0.06f };
  static constexpr SubParams FLICKER { 0.11f, 0.75f, 0.14f,  0.60f, 0.35f, 0.18f };
  static constexpr SubParams WIND    { 0.13f, 0.92f, 0.28f,  1.20f, 0.50f, 0.22f };
  static constexpr float WIND_GUST_SPEED = 0.006f;
  static constexpr float WIND_GUST_DEPTH  = 0.85f;

  enum class Sub : uint8_t { CALM, FLICKER, WIND };

  struct State {
    Sub sub = Sub::FLICKER;
    Sub next_sub = Sub::FLICKER;
    unsigned long sub_end = 0;
    unsigned long xfade_end = 0;
    float t_fast = 0, t_fast2 = 0, t_gust = 0, t_w1 = 0, t_w2 = 0;
    float snuff_w1 = 1.0f, snuff_w2 = 1.0f;
    float out_w1 = 0.08f, out_w2 = 0.08f, out_red = 0.05f;
    unsigned long last = 0;
  };
  static State s;

  static Sub pick_next(Sub cur) {
    struct { Sub m; int w; } t[3] = {{Sub::CALM,PCT_CALM},{Sub::FLICKER,PCT_FLICKER},{Sub::WIND,PCT_WIND}};
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
      s.sub = Sub::FLICKER;
      s.next_sub = pick_next(s.sub);
      s.sub_end = millis() + rnd(SUBMODE_MIN_MS, SUBMODE_MAX_MS);
      s.xfade_end = 0;
      s.t_fast = rnd(0, 10000); s.t_fast2 = rnd(10000, 30000);
      s.t_gust = rnd(0, 10000);
      s.t_w1 = rnd(0, 10000); s.t_w2 = rnd(20000, 40000);
      s.snuff_w1 = s.snuff_w2 = 1.0f;
      s.out_w1 = s.out_w2 = 0.08f; s.out_red = 0.05f;
      s.last = millis();
    }
    unsigned long now = millis();
    unsigned long dt = now - s.last;
    if (dt == 0) return;
    s.last = now;
    float dtf = (float) dt;

    // Sub-mode scheduler with crossfade.
    if (now >= s.sub_end && s.xfade_end == 0) {
      s.next_sub = pick_next(s.sub);
      s.xfade_end = now + XFADE_MS;
      s.sub_end = s.xfade_end + rnd(SUBMODE_MIN_MS, SUBMODE_MAX_MS);
    }
    if (s.xfade_end != 0 && now >= s.xfade_end) { s.sub = s.next_sub; s.xfade_end = 0; }

    // Advance noise axes.
    const SubParams& p = params(s.sub);
    s.t_fast  += p.speed  * dtf;
    s.t_fast2 += p.speed2 * dtf;
    s.t_gust  += WIND_GUST_SPEED * dtf;
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

    // Snuff events (flicker only).
    if (s.sub == Sub::FLICKER) {
      if (s.snuff_w1 < 1.0f || rnd(0, 1000) < SNUFF_CHANCE) {
        if (s.snuff_w1 >= 1.0f) s.snuff_w1 = 1.0f - SNUFF_DEPTH;
        s.snuff_w1 += (1.0f - s.snuff_w1) * clampf(SNUFF_RECOVER * dtf);
      }
      if (s.snuff_w2 < 1.0f || rnd(0, 1000) < SNUFF_CHANCE) {
        if (s.snuff_w2 >= 1.0f) s.snuff_w2 = 1.0f - SNUFF_DEPTH;
        s.snuff_w2 += (1.0f - s.snuff_w2) * clampf(SNUFF_RECOVER * dtf);
      }
    } else { s.snuff_w1 = s.snuff_w2 = 1.0f; }
    target_w1 *= s.snuff_w1;
    target_w2 *= s.snuff_w2;

    // Red ember: inverse curve over its own cap, scaled to fraction of MAX_DUTY.
    float peak = params(s.sub).brightness;
    float dimness = 1.0f - clampf(cur_level / peak);
    float red_of_cap = RED_MIN + (RED_MAX - RED_MIN) * powf(dimness, RED_CURVE);
    float target_red = red_of_cap;  // red ledc output already caps via max_power

    // Exponential smoothing.
    float alpha = clampf(p.smoothing * dtf);
    s.out_w1  += (target_w1  - s.out_w1)  * alpha;
    s.out_w2  += (target_w2  - s.out_w2)  * alpha;
    s.out_red += (target_red - s.out_red) * alpha;

    // Drive channels. ledc set_level takes 0..1; max_power applies the cap.
    id(pwm_led_1).set_level(clampf(s.out_w1));
    id(pwm_led_2).set_level(clampf(s.out_w2));
    id(pwm_led_4).set_level(clampf(s.out_red));
    id(pwm_led_3).set_level(0.0f);  // UV off in candle mode

    // RGB strip off.
    it.all() = esphome::Color(0, 0, 0);
  }
}  // namespace candle

// ===========================================================================
// COLOR - slow HSV drift with a speed random-walk and a spatial history
// buffer for a spread of hues along the strip. Spatial blur softens edges.
//
// Drives: RGB strip. White/UV/red off.
// ===========================================================================
namespace color {
  static constexpr float SPEED_MIN = 0.002f;
  static constexpr float SPEED_MAX = 0.12f;
  static constexpr int   SATURATION = 240;
  static constexpr int   BRIGHTNESS = 160;

  struct State {
    float hue = 0;
    float speed = 0.015f;
    float vel = 0;
    unsigned long last = 0;
  };
  static State s;
  static constexpr int HIST = 20 + 5;
  static uint8_t hist[HIST];
  static int hist_idx = 0;
  static unsigned long last_push = 0;

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    if (initial) {
      s.hue = rnd(0, 256);
      s.speed = 0.015f; s.vel = 0;
      s.last = millis(); last_push = millis(); hist_idx = 0;
      for (int i = 0; i < HIST; i++) hist[i] = (uint8_t) s.hue;
    }
    unsigned long now = millis();
    unsigned long dt = now - s.last;
    if (dt < 20) return;
    s.last = now;
    float dtf = (float) dt;

    // Speed random walk with centre pull.
    float centre = (SPEED_MIN + SPEED_MAX) * 0.5f;
    s.vel += (((float) rnd(0, 1000) / 500.0f) - 1.0f) * 0.000003f * dtf;
    s.vel *= 0.97f;
    s.vel += (centre - s.speed) * 0.000008f * dtf;
    s.speed += s.vel * dtf;
    if (s.speed < SPEED_MIN) s.speed = SPEED_MIN;
    if (s.speed > SPEED_MAX) s.speed = SPEED_MAX;

    s.hue += s.speed * dtf;
    if (s.hue >= 256.0f) s.hue -= 256.0f;

    unsigned long interval = (unsigned long)(150.0f / (s.speed / 0.015f));
    if (interval < 30) interval = 30;
    if (interval > 2000) interval = 2000;
    if (now - last_push > interval) {
      hist[hist_idx] = (uint8_t) s.hue;
      hist_idx = (hist_idx + 1) % HIST;
      last_push = now;
    }

    int n = it.size();
    for (int i = 0; i < n && i < 20; i++) {
      int pos = (hist_idx - i + HIST) % HIST;
      uint8_t h = hist[pos] + (uint8_t) rnd(0, 4);
      it[i] = esphome::light::ESPHSVColor(h, SATURATION, BRIGHTNESS);
    }
    // Spatial blur.
    for (int i = 0; i < n && i < 20; i++) {
      int prev = (i - 1 + 20) % 20;
      int next = (i + 1) % 20;
      esphome::Color c = it[i].get() * 179;
      esphome::Color p = it[prev].get() * 38;
      esphome::Color nx = it[next].get() * 38;
      it[i] = c + p + nx;
    }

    // Mono channels off.
    id(pwm_led_1).set_level(0.0f);
    id(pwm_led_2).set_level(0.0f);
    id(pwm_led_3).set_level(0.0f);
    id(pwm_led_4).set_level(0.0f);
  }
}  // namespace color

// ===========================================================================
// MAGIC - alternating DRIFT and SPARK phases in the purple/blue hue range,
// with the UV channel on and the red LED on a slow independent drift.
//
// Drives: RGB strip + UV + red. White off.
// ===========================================================================
namespace magic {
  static constexpr float HUE_CENTER = 175.0f;
  static constexpr float HUE_SPREAD = 40.0f;
  static constexpr unsigned long DRIFT_MIN = 5000, DRIFT_MAX = 20000;
  static constexpr unsigned long SPARK_MIN  = 2000, SPARK_MAX  = 6000;
  static constexpr float DRIFT_SPEED_MIN = 0.003f, DRIFT_SPEED_MAX = 0.021f;
  static constexpr int   DRIFT_BRIGHTNESS = 55;
  static constexpr int   DRIFT_RIPPLE     = 20;
  static constexpr int   SPARK_FADE       = 210;
  static constexpr int   SPARK_TINT_V     = 18;
  static constexpr int   SPARK_CHANCE     = 8;
  static constexpr int   SPARK_BRI_MIN    = 200, SPARK_BRI_MAX = 255;
  static constexpr int   SPARK_HUE_SPREAD = 20;
  static constexpr float RED_STEP = 0.000006f, RED_DAMP = 0.985f;
  static constexpr float RED_CENTRE = 0.45f, RED_PULL = 0.000008f;
  static constexpr float RED_MIN = 0.15f, RED_MAX = 0.85f;

  enum class Phase { DRIFT, SPARK };
  struct State {
    Phase phase = Phase::DRIFT;
    unsigned long phase_end = 0;
    float drift_hue = 160.0f, drift_speed = 0.01f;
    float red_level = 0.4f, red_vel = 0;
    unsigned long last = 0;
  };
  static State s;

  static void start_phase() {
    unsigned long now = millis();
    if (s.phase == Phase::DRIFT) {
      s.phase = Phase::SPARK;
      s.phase_end = now + rnd(SPARK_MIN, SPARK_MAX);
    } else {
      s.phase = Phase::DRIFT;
      s.phase_end = now + rnd(DRIFT_MIN, DRIFT_MAX);
      s.drift_speed = DRIFT_SPEED_MIN +
                      ((float) rnd(0, 1000) / 1000.0f) * (DRIFT_SPEED_MAX - DRIFT_SPEED_MIN);
    }
  }

  inline void tick(esphome::light::AddressableLight &it, bool initial) {
    if (initial) {
      unsigned long now = millis();
      s.phase = Phase::DRIFT;
      s.phase_end = now + rnd(DRIFT_MIN, DRIFT_MAX);
      s.drift_hue = HUE_CENTER + (float) rnd(0, (int) HUE_SPREAD);
      s.drift_speed = (DRIFT_SPEED_MIN + DRIFT_SPEED_MAX) * 0.5f;
      s.red_level = 0.4f; s.red_vel = 0;
      s.last = now;
    }
    unsigned long now = millis();
    unsigned long dt = now - s.last;
    if (dt == 0) return;
    s.last = now;
    float dtf = (float) dt;

    if (now >= s.phase_end) start_phase();

    // Red LED slow drift independent of phase.
    s.red_vel += (((float) rnd(0, 1000) / 500.0f) - 1.0f) * RED_STEP * dtf;
    s.red_vel *= RED_DAMP;
    s.red_vel += (RED_CENTRE - s.red_level) * RED_PULL * dtf;
    s.red_level += s.red_vel * dtf;
    if (s.red_level < RED_MIN) s.red_level = RED_MIN;
    if (s.red_level > RED_MAX) s.red_level = RED_MAX;
    id(pwm_led_4).set_level(s.red_level);
    id(pwm_led_3).set_level(1.0f);  // UV on (max_power caps it)

    int n = it.size();
    if (s.phase == Phase::DRIFT) {
      s.drift_hue += s.drift_speed * dtf;
      if (s.drift_hue >= 256.0f) s.drift_hue -= 256.0f;
      for (int i = 0; i < n && i < 20; i++) {
        float ripple = sinf((float) i * 0.45f + s.drift_hue * 0.025f) * 6.0f;
        uint8_t h = (uint8_t)(s.drift_hue + ripple);
        uint8_t v = DRIFT_BRIGHTNESS +
                    (uint8_t)(sinf((float) i * 0.3f + s.drift_hue * 0.018f) * DRIFT_RIPPLE);
        it[i] = esphome::light::ESPHSVColor(h, 230, v);
      }
    } else {
      // SPARK: fade down + occasional sparks.
      for (int i = 0; i < n && i < 20; i++) {
        it[i].fade_to_black(SPARK_FADE);
        it[i] = it[i].get() + esphome::light::ESPHSVColor((uint8_t) s.drift_hue, 220, SPARK_TINT_V).to_rgb();
      }
      if (rnd(0, 100) < SPARK_CHANCE) {
        int pos = rnd(0, 20);
        uint8_t sh = (uint8_t)(s.drift_hue + rnd(0, SPARK_HUE_SPREAD * 2) - SPARK_HUE_SPREAD);
        uint8_t sv = SPARK_BRI_MIN + rnd(0, SPARK_BRI_MAX - SPARK_BRI_MIN);
        it[pos] = esphome::light::ESPHSVColor(sh, 200, sv);
      }
      s.drift_hue += 0.004f * dtf;
      if (s.drift_hue >= 256.0f) s.drift_hue -= 256.0f;
    }

    // Whites off.
    id(pwm_led_1).set_level(0.0f);
    id(pwm_led_2).set_level(0.0f);
  }
}  // namespace magic

}  // namespace espcandle
