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
  // Brightness ~15% below the original port (still reads as a tea-light flame).
  struct SubParams {
    float brightness, depth, speed, speed2, octave2, smoothing;
  };
  static constexpr SubParams CALM    { 0.06f, 0.60f, 0.018f, 0.07f, 0.35f, 0.06f };
  static constexpr SubParams FLICKER { 0.09f, 0.75f, 0.14f,  0.60f, 0.35f, 0.18f };
  static constexpr SubParams WIND    { 0.11f, 0.92f, 0.28f,  1.20f, 0.50f, 0.22f };
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
      it.all() = esphome::Color(0, 0, 0);
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

    id(pwm_led_1).set_level(clampf(s.out_w));
    id(pwm_led_2).set_level(clampf(s.out_w * 0.85f));
    id(pwm_led_4).set_level(clampf(s.out_red));
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
  static constexpr int   SATURATION = 240;
  static constexpr int   BRIGHTNESS = 150;
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
  static constexpr int   DRIFT_BRIGHTNESS = 38;
  static constexpr int   DRIFT_RIPPLE     = 16;
  static constexpr int   SPARK_BRI_MIN    = 170, SPARK_BRI_MAX = 230;
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
    id(pwm_led_4).set_level(s.red_level);
    id(pwm_led_3).set_level(0.55f);

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

    id(pwm_led_1).set_level(0.0f);
    id(pwm_led_2).set_level(0.0f);
  }
}  // namespace magic

}  // namespace espcandle
