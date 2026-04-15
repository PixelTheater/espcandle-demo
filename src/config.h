#pragma once
#include <stdint.h>

// ─── Hardware ─────────────────────────────────────────────────────────────────

// PWM LED pin indices into LED_PINS[]
inline constexpr int LED_PINS[4]  = {15, 16, 17, 18};
inline constexpr int WHITE_LED_1  = 0;   // GPIO15
inline constexpr int WHITE_LED_2  = 1;   // GPIO16
inline constexpr int UV_LED       = 2;   // GPIO17
inline constexpr int RED_LED      = 3;   // GPIO18
inline constexpr int WS2812_PIN   = 33;
inline constexpr int BUTTON_PIN   = 0;

// PWM carrier frequency and resolution for mono LED channels.
// Higher resolution = more brightness steps = no staircase at low brightness.
// ESP32-S3 LEDC supports up to 14-bit at 5 kHz.  12-bit gives 4095 steps.
inline constexpr double  PWM_FREQ       = 5000.0;
inline constexpr uint8_t PWM_RESOLUTION = 12;
inline constexpr int     MAX_DUTY       = (1 << PWM_RESOLUTION) - 1; // 4095

// Per-channel hard brightness caps (0–100 %).
// PWM duty never exceeds dutyFromPercent(cap), regardless of mode requests.
inline constexpr int BRIGHTNESS_MAX_WHITE = 40;
inline constexpr int BRIGHTNESS_MAX_UV    = 75;
inline constexpr int BRIGHTNESS_MAX_RED   = 80;

// WS2812 strip
inline constexpr int NUM_LEDS            = 20;
inline constexpr int WS2812_BRIGHTNESS   = 64;  // global FastLED brightness (0–255)

// Button
inline constexpr unsigned long LONG_PRESS_TIME = 3000; // ms

// Helper: convert 0–100 % to a PWM duty count
inline constexpr int dutyFromPercent(int pct) {
    return (MAX_DUTY * pct) / 100;
}


// ─── Candle mode ──────────────────────────────────────────────────────────────
//
// The candle animation cycles through three sub-modes: CALM, FLICKER, WIND.
// Each sub-mode uses 1D Perlin noise (FastLED inoise8) sampled at a moving
// time position to produce organic, smooth brightness variation.
//
// Brightness values are 0.0–1.0 fractions of MAX_DUTY.
// Speed values are Perlin time units advanced per millisecond.
// Smoothing is an output lerp factor per ms (lower = slower / more sluggish).
//
// Sub-mode time allocation: PCT values must sum to 100.
// Each sub-mode runs for a random duration within its MIN/MAX window (ms),
// and the scheduler weights selection by these percentages.
// Transitions between sub-modes crossfade over CANDLE_XFADE_MS.

inline constexpr int   CANDLE_PCT_CALM    = 45;   // % of time in calm mode
inline constexpr int   CANDLE_PCT_FLICKER = 45;   // % of time in flicker mode
inline constexpr int   CANDLE_PCT_WIND    = 10;   // % of time in wind mode

inline constexpr unsigned long CANDLE_SUBMODE_MIN_MS = 4000;   // shortest sub-mode run
inline constexpr unsigned long CANDLE_SUBMODE_MAX_MS = 20000;  // longest sub-mode run
inline constexpr unsigned long CANDLE_XFADE_MS       = 600;    // crossfade between sub-modes

// Red LED — expressed as a fraction of BRIGHTNESS_MAX_RED (its own cap), not MAX_DUTY,
// so the full visible range of the LED is used.
// RED_MIN  : ember glow when flame is at peak brightness  (0–1 of its cap)
// RED_MAX  : ember glow when flame is near zero           (0–1 of its cap)
// RED_CURVE: exponent on dimness — higher = sharper roll to RED_MAX at low levels
inline constexpr float CANDLE_RED_MIN   = 0.10f;  // 10% of cap at full flame — subtle warm tint
inline constexpr float CANDLE_RED_MAX   = 0.35f;  // 85% of cap when nearly out — vivid ember
inline constexpr float CANDLE_RED_CURVE = 1.2f;   // shape of roll-off

// W1/W2 — each channel has its own Perlin axis at a different speed so they
// drift at different rates and never lock together.
inline constexpr float CANDLE_W1_SPEED     = 0.022f;  // Perlin advance per ms for W1
inline constexpr float CANDLE_W2_SPEED     = 0.037f;  // different rate ensures desync
inline constexpr float CANDLE_SPLIT_DEPTH  = 0.50f;   // ± per-channel drift in calm (fraction of curLevel)
inline constexpr float CANDLE_SPLIT_DEPTH_FLICKER = 0.25f; // smaller but still visible in flicker/wind

// Snuff events (flicker mode only) — one white channel briefly dips near zero
// then recovers, like a gas pocket momentarily killing the flame.
// SNUFF_CHANCE : probability per frame of triggering (0–1000 scale, checked each update)
// SNUFF_DEPTH  : how far the channel dips (1.0 = full off)
// SNUFF_RECOVER: smoothing factor for recovery — higher = snappier return
inline constexpr int   CANDLE_SNUFF_CHANCE   = 2;     // out of 1000 per frame
inline constexpr float CANDLE_SNUFF_DEPTH    = 0.97f; // dip to ~3% of normal
inline constexpr float CANDLE_SNUFF_RECOVER  = 0.08f; // lerp per ms back to normal

// ── CALM sub-mode ─────────────────────────────────────────────────────────────
// Smooth, barely-moving flame. Low Perlin speed, narrow brightness band.
// A second octave at 3x speed and 25% weight adds gentle irregularity.
inline constexpr float CANDLE_CALM_BRIGHTNESS  = 0.07f;  // peak output (fraction of MAX_DUTY)
inline constexpr float CANDLE_CALM_DEPTH       = 0.60f;  // noise modulation depth (floor = peak*(1-depth))
inline constexpr float CANDLE_CALM_SPEED       = 0.018f; // primary Perlin advance per ms
inline constexpr float CANDLE_CALM_SPEED2      = 0.07f;  // secondary octave speed
inline constexpr float CANDLE_CALM_OCTAVE2     = 0.35f;  // secondary octave weight (0=off)
inline constexpr float CANDLE_CALM_SMOOTHING   = 0.06f;  // output lerp per ms  (~17 ms TC)

// ── FLICKER sub-mode ──────────────────────────────────────────────────────────
// Natural candle — medium speed variation over a wider brightness range.
// Two octaves: a slow swell + fast crackle on top.
inline constexpr float CANDLE_FLICKER_BRIGHTNESS = 0.11f;
inline constexpr float CANDLE_FLICKER_DEPTH      = 0.75f;  // floor = ~2.75% of MAX_DUTY
inline constexpr float CANDLE_FLICKER_SPEED      = 0.14f;  // primary Perlin advance per ms
inline constexpr float CANDLE_FLICKER_SPEED2     = 0.60f;  // secondary octave speed
inline constexpr float CANDLE_FLICKER_OCTAVE2    = 0.35f;  // secondary octave weight
inline constexpr float CANDLE_FLICKER_SMOOTHING  = 0.18f;  // output lerp per ms  (~7 ms TC)

// ── WIND sub-mode ─────────────────────────────────────────────────────────────
// Agitated flame — fast high-amplitude noise; a second slow Perlin layer acts
// as a gust envelope that can pull the flame near-out for several seconds.
inline constexpr float CANDLE_WIND_BRIGHTNESS  = 0.13f;
inline constexpr float CANDLE_WIND_DEPTH       = 0.92f;   // fast noise depth — swings nearly to zero
inline constexpr float CANDLE_WIND_SPEED       = 0.28f;   // fast Perlin advance per ms
inline constexpr float CANDLE_WIND_SPEED2      = 1.20f;   // crackle octave speed — rapid oscillation
inline constexpr float CANDLE_WIND_OCTAVE2     = 0.50f;   // crackle octave weight
inline constexpr float CANDLE_WIND_SMOOTHING   = 0.22f;   // output lerp per ms — follows fast swings
inline constexpr float CANDLE_WIND_GUST_SPEED  = 0.006f;  // slow envelope Perlin per ms
inline constexpr float CANDLE_WIND_GUST_DEPTH  = 0.85f;   // gust can pull flame very low


// ─── Color mode ───────────────────────────────────────────────────────────────
//
// Hue cycle speed drifts over time between slow and fast.
// Speed unit: hue units (0–255 scale) per millisecond.

inline constexpr float COLOR_SPEED_MIN    = 0.002f; // ~2 min per full cycle — near static
inline constexpr float COLOR_SPEED_MAX    = 0.12f;  // ~3 s per full cycle — visibly moving
inline constexpr int   COLOR_BRIGHTNESS   = 160;    // WS2812 value channel (0–255)
inline constexpr int   COLOR_SATURATION   = 240;    // WS2812 saturation (0–255)


// ─── Magic mode ───────────────────────────────────────────────────────────────

// Hue range for the drift phase (purple/blue region of HSV wheel, 0–255)
inline constexpr float MAGIC_HUE_CENTER  = 175.0f; // starting hue offset
inline constexpr float MAGIC_HUE_SPREAD  =  40.0f; // random spread on entry

// Drift phase: slow atmospheric hue sweep
inline constexpr unsigned long MAGIC_DRIFT_PHASE_MIN = 5000;  // ms
inline constexpr unsigned long MAGIC_DRIFT_PHASE_MAX = 20000;
inline constexpr float         MAGIC_DRIFT_SPEED_MIN = 0.003f; // hue units/ms
inline constexpr float         MAGIC_DRIFT_SPEED_MAX = 0.021f;
inline constexpr int           MAGIC_DRIFT_BRIGHTNESS = 55;    // base WS2812 value
inline constexpr int           MAGIC_DRIFT_RIPPLE     = 20;    // ± brightness ripple per LED

// Spark phase: dim background with bright sparks
inline constexpr unsigned long MAGIC_SPARK_PHASE_MIN  = 2000;  // ms
inline constexpr unsigned long MAGIC_SPARK_PHASE_MAX  = 6000;
inline constexpr int           MAGIC_SPARK_FADE       = 210;   // nscale8 decay per frame (0–255)
inline constexpr int           MAGIC_SPARK_TINT_V     = 18;    // dim tint brightness added per frame
inline constexpr int           MAGIC_SPARK_CHANCE     = 8;     // % chance per frame to spawn a spark
inline constexpr int           MAGIC_SPARK_BRIGHTNESS_MIN = 200;
inline constexpr int           MAGIC_SPARK_BRIGHTNESS_MAX = 255;
inline constexpr int           MAGIC_SPARK_HUE_SPREAD = 20;    // ± hue variation around drift hue

// Red LED drift in magic mode (independent slow random walk)
inline constexpr float MAGIC_RED_STEP    = 0.000006f; // random-walk step per ms
inline constexpr float MAGIC_RED_DAMPING = 0.985f;    // velocity damping per ms
inline constexpr float MAGIC_RED_CENTRE  = 0.45f;     // resting level (fraction)
inline constexpr float MAGIC_RED_PULL    = 0.000008f; // centre-pull strength per ms
inline constexpr float MAGIC_RED_MIN     = 0.15f;     // minimum level
inline constexpr float MAGIC_RED_MAX     = 0.85f;     // maximum level
