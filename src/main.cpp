#include <Arduino.h>
#include <FastLED.h>
#include "config.h"
#include "types.h"
#include "cli.h"

// WS2812 LED array
CRGB leds[NUM_LEDS];
bool lastButtonState = HIGH;
unsigned long buttonPressStart = 0;
bool buttonPressed = false;

// Mode management (enum defined in types.h)

CandleMode currentMode = CANDLE_MODE;
CandleMode lastActiveMode = CANDLE_MODE;
bool powerOn = true;

// Mode-specific variables
unsigned long lastAutoModeChange = 0;

// ── Candle simulation ────────────────────────────────────────────────────────
//
// Three sub-modes (CALM, FLICKER, WIND) cycle according to configured time
// percentages. Each samples 1D Perlin noise (FastLED inoise8) at a moving
// time position — smooth by construction, no explicit filter needed for
// CALM/FLICKER. WIND adds a second slower Perlin layer as a gust envelope.
//
// A slow third Perlin axis drives a small W1/W2 split so the two whites are
// never perfectly synchronised.
//
// All parameters live in config.h. No magic numbers here.

enum class FlameSubMode : uint8_t { CALM, FLICKER, WIND };

struct CandleState {
    // Sub-mode scheduler
    FlameSubMode subMode     = FlameSubMode::FLICKER;
    FlameSubMode nextSubMode = FlameSubMode::FLICKER;
    unsigned long subModeEnd  = 0;   // millis() when current sub-mode ends
    unsigned long xfadeEnd    = 0;   // millis() when crossfade ends (0 = no xfade)

    // Per-mode Perlin time accumulators (float for sub-ms precision)
    float tFast  = 0.0f;   // primary noise axis
    float tFast2 = 0.0f;   // secondary octave axis
    float tGust  = 0.0f;   // slow wind-gust envelope axis (WIND only)
    float tW1    = 0.0f;   // W1 independent drift axis
    float tW2    = 0.0f;   // W2 independent drift axis (started in different noise region)

    // Snuff state — per-channel momentary dip in flicker mode
    float snuffW1 = 1.0f;  // multiplier: 1.0 = normal, <1 = snuffed
    float snuffW2 = 1.0f;

    // Smoothed output levels (0.0–1.0 fraction of MAX_DUTY)
    float outW1  = 0.20f;
    float outW2  = 0.20f;
    float outRed = 0.08f;

    unsigned long lastUpdate = 0;
};

static CandleState candleState;

// Blend two rescaled inoise8 samples into a brightness level for a sub-mode.
// n1/n2 are already rescaled 0–255; gustVal drives the WIND envelope.
static float noiseToLevel(FlameSubMode m, uint8_t n1, uint8_t n2, uint8_t gustVal = 128) {
    switch (m) {
        case FlameSubMode::CALM: {
            float n = (n1 * (1.0f - CANDLE_CALM_OCTAVE2) + n2 * CANDLE_CALM_OCTAVE2) / 255.0f;
            float base = CANDLE_CALM_BRIGHTNESS * (1.0f - CANDLE_CALM_DEPTH);
            return base + CANDLE_CALM_BRIGHTNESS * CANDLE_CALM_DEPTH * n;
        }
        case FlameSubMode::FLICKER: {
            float n = (n1 * (1.0f - CANDLE_FLICKER_OCTAVE2) + n2 * CANDLE_FLICKER_OCTAVE2) / 255.0f;
            float base = CANDLE_FLICKER_BRIGHTNESS * (1.0f - CANDLE_FLICKER_DEPTH);
            return base + CANDLE_FLICKER_BRIGHTNESS * CANDLE_FLICKER_DEPTH * n;
        }
        case FlameSubMode::WIND: {
            float n = (n1 * (1.0f - CANDLE_WIND_OCTAVE2) + n2 * CANDLE_WIND_OCTAVE2) / 255.0f;
            float gust = gustVal / 255.0f;
            float envelope = 1.0f - CANDLE_WIND_GUST_DEPTH * (1.0f - gust);
            float base = CANDLE_WIND_BRIGHTNESS * (1.0f - CANDLE_WIND_DEPTH);
            return (base + CANDLE_WIND_BRIGHTNESS * CANDLE_WIND_DEPTH * n) * envelope;
        }
    }
    return 0.0f;
}

// Pick the next sub-mode weighted by the PCT constants.
static FlameSubMode pickNextSubMode(FlameSubMode current) {
    // Build weighted table excluding current
    struct { FlameSubMode m; int w; } table[3] = {
        { FlameSubMode::CALM,    CANDLE_PCT_CALM    },
        { FlameSubMode::FLICKER, CANDLE_PCT_FLICKER },
        { FlameSubMode::WIND,    CANDLE_PCT_WIND    },
    };
    int total = 0;
    for (auto& e : table) if (e.m != current) total += e.w;
    if (total == 0) return current;
    int r = random(0, total);
    int acc = 0;
    for (auto& e : table) {
        if (e.m == current) continue;
        acc += e.w;
        if (r < acc) return e.m;
    }
    return FlameSubMode::FLICKER;
}

static float smoothingForMode(FlameSubMode m) {
    switch (m) {
        case FlameSubMode::CALM:    return CANDLE_CALM_SMOOTHING;
        case FlameSubMode::FLICKER: return CANDLE_FLICKER_SMOOTHING;
        case FlameSubMode::WIND:    return CANDLE_WIND_SMOOTHING;
    }
    return CANDLE_FLICKER_SMOOTHING;
}

// Color mode state
static float         colorHue          = 0.0f;  // 0.0–255.0 hue accumulator
static float         colorCycleSpeed   = 0.02f; // hue units per ms — drifts over time
static float         colorSpeedVel     = 0.0f;  // speed random-walk velocity
static unsigned long lastColorUpdate   = 0;
const int COLOR_HISTORY_SIZE = NUM_LEDS + 5;
static uint8_t colorHistory[COLOR_HISTORY_SIZE];
static int     colorHistoryIndex  = 0;
static unsigned long lastHistoryPush = 0;

// Magic mode state
enum class MagicPhase { DRIFT, SPARK };
struct MagicState {
    MagicPhase   phase       = MagicPhase::DRIFT;
    unsigned long phaseEnd   = 0;      // millis() when current phase ends
    float        driftHue    = 160.0f; // current hue for drift phase (purple/blue range)
    float        driftSpeed  = 0.01f;  // hue units per ms
    float        redLevel    = 0.5f;   // red LED brightness 0–1 (relative to max)
    float        redVel      = 0.0f;   // red drift velocity
    unsigned long lastUpdate = 0;
};
static MagicState magicState;

CandleMode currentAutoMode = CANDLE_MODE;

// ModeConfig struct defined in types.h

// Forward declarations
void updateCandleMode();
void updateColorMode();
void updateMagicMode();
void updateAutoMode();
void enterCandleMode();
void enterColorMode();
void enterMagicMode();
void enterAutoMode();
void exitCandleMode();
void exitColorMode();
void exitMagicMode();
void exitAutoMode();
void handleButton();
void turnOffAllLEDs();
void setPWMBrightness(int pin, int brightness);

// Mode configurations
extern const ModeConfig MODES[NUM_MODES];
const ModeConfig MODES[NUM_MODES] = {
    {"Candle", updateCandleMode, enterCandleMode, exitCandleMode},
    {"Color", updateColorMode, enterColorMode, exitColorMode},
    {"Magic", updateMagicMode, enterMagicMode, exitMagicMode},
    {"Auto", updateAutoMode, enterAutoMode, exitAutoMode}
};

void setup() {
    Serial.begin(115200);

    Serial.println("\n=== ESP Candle ===");
    Serial.printf("Chip: %s  Rev: %d  Cores: %d  CPU: %d MHz\n",
        ESP.getChipModel(), ESP.getChipRevision(),
        ESP.getChipCores(), getCpuFrequencyMhz());
    Serial.printf("Flash: %d KB  Free heap: %d B\n",
        ESP.getFlashChipSize() / 1024, ESP.getFreeHeap());
    Serial.printf("PWM freq: %.0f Hz  Resolution: %d-bit  Max duty: %d\n",
        PWM_FREQ, PWM_RESOLUTION, MAX_DUTY);
    Serial.printf("WS2812 pin: %d  LEDs: %d\n", WS2812_PIN, NUM_LEDS);
    Serial.printf("Button pin: %d  Long press: %lums\n", BUTTON_PIN, LONG_PRESS_TIME);
    Serial.printf("PWM pins - White1: %d  White2: %d  UV: %d  Red: %d\n",
        LED_PINS[WHITE_LED_1], LED_PINS[WHITE_LED_2], LED_PINS[UV_LED], LED_PINS[RED_LED]);
    Serial.printf("Brightness limits - white: %d%%  uv: %d%%  red: %d%%\n",
        BRIGHTNESS_MAX_WHITE, BRIGHTNESS_MAX_UV, BRIGHTNESS_MAX_RED);
    Serial.println("------------------");

    // Initialize button
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // Initialize PWM LEDs
    for (int i = 0; i < 4; i++) {
        ledcAttach(LED_PINS[i], PWM_FREQ, PWM_RESOLUTION);
        ledcWrite(LED_PINS[i], 0);
    }

    // Initialize WS2812 LEDs
    FastLED.addLeds<WS2812, WS2812_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(WS2812_BRIGHTNESS);

    // Enter initial mode
    Serial.printf("Starting in mode: %s\n", MODES[currentMode].name);
    if (MODES[currentMode].enterFunction) {
        MODES[currentMode].enterFunction();
    }

    Serial.println("Ready.");
    cliBegin();
}

void loop() {
    cliUpdate();
    handleButton();

    if (!cliTestActive() && powerOn && MODES[currentMode].updateFunction) {
        MODES[currentMode].updateFunction();
    }

    FastLED.show(); // Update WS2812 LEDs

    // Periodic status heartbeat every 30 seconds
    static unsigned long lastStatusPrint = 0;
    unsigned long now = millis();
    if (now - lastStatusPrint >= 30000) {
        Serial.printf("[STATUS] uptime=%lus  power=%s  mode=%s  heap=%dB  temp=%.1fC\n",
            now / 1000,
            powerOn ? "ON" : "OFF",
            MODES[currentMode].name,
            ESP.getFreeHeap(),
            temperatureRead());
        lastStatusPrint = now;
    }

    // Yield to background tasks (WiFi stack, watchdog) without a fixed sleep.
    // Animation timing is driven by millis() deltas — no frame-rate cap needed.
    yield();
}

void handleButton() {
    bool buttonState = digitalRead(BUTTON_PIN);

    // Button press detected (falling edge)
    if (lastButtonState == HIGH && buttonState == LOW) {
        buttonPressStart = millis();
        buttonPressed = true;
        Serial.printf("[BTN] Pressed  (t=%lums)\n", buttonPressStart);
    }

    // Long-press threshold crossed while still held
    static bool longPressReported = false;
    if (buttonPressed && buttonState == LOW) {
        unsigned long heldFor = millis() - buttonPressStart;
        if (heldFor >= LONG_PRESS_TIME && !longPressReported) {
            Serial.printf("[BTN] Long-press threshold reached (%lums)\n", heldFor);
            longPressReported = true;
        }
    }
    if (!buttonPressed) {
        longPressReported = false;
    }

    // Button release detected (rising edge)
    if (lastButtonState == LOW && buttonState == HIGH && buttonPressed) {
        unsigned long pressDuration = millis() - buttonPressStart;
        buttonPressed = false;

        Serial.printf("[BTN] Released  duration=%lums  -> %s\n",
            pressDuration, pressDuration < LONG_PRESS_TIME ? "SHORT" : "LONG");

        if (pressDuration < LONG_PRESS_TIME) {
            // Short press - mode change or power on
            if (powerOn) {
                CandleMode prevMode = currentMode;

                if (MODES[currentMode].exitFunction) {
                    MODES[currentMode].exitFunction();
                }

                currentMode = (CandleMode)((currentMode + 1) % NUM_MODES);
                lastActiveMode = currentMode;

                if (MODES[currentMode].enterFunction) {
                    MODES[currentMode].enterFunction();
                }

                Serial.printf("[MODE] %s -> %s\n",
                    MODES[prevMode].name, MODES[currentMode].name);
            } else {
                powerOn = true;
                currentMode = lastActiveMode;
                if (MODES[currentMode].enterFunction) {
                    MODES[currentMode].enterFunction();
                }
                Serial.printf("[PWR] ON  restoring mode: %s\n", MODES[currentMode].name);
            }
        } else {
            // Long press - power off
            powerOn = false;
            turnOffAllLEDs();
            Serial.printf("[PWR] OFF  (was in mode: %s)\n", MODES[currentMode].name);
        }
    }

    lastButtonState = buttonState;
}

void turnOffAllLEDs() {
    // Turn off all PWM LEDs
    for (int i = 0; i < 4; i++) {
        ledcWrite(LED_PINS[i], 0);
    }
    
    // Turn off WS2812 LEDs
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

void setPWMBrightness(int ledIndex, int brightness) {
    int maxPct = (ledIndex == UV_LED)  ? BRIGHTNESS_MAX_UV
               : (ledIndex == RED_LED) ? BRIGHTNESS_MAX_RED
               :                        BRIGHTNESS_MAX_WHITE;
    brightness = constrain(brightness, 0, dutyFromPercent(maxPct));
    ledcWrite(LED_PINS[ledIndex], brightness);
}

// ── Candle Mode ───────────────────────────────────────────────────────────────

void enterCandleMode() {
    CandleState& s   = candleState;
    unsigned long now = millis();
    s.subMode     = FlameSubMode::FLICKER;
    s.nextSubMode = pickNextSubMode(s.subMode);
    s.subModeEnd  = now + random(CANDLE_SUBMODE_MIN_MS, CANDLE_SUBMODE_MAX_MS);
    s.xfadeEnd    = 0;
    s.tFast       = (float)random(0, 10000);
    s.tFast2      = (float)random(10000, 30000);
    s.tGust       = (float)random(0, 10000);
    s.tW1         = (float)random(0, 10000);
    s.tW2         = (float)random(20000, 40000);  // well-separated noise region
    s.snuffW1     = 1.0f;
    s.snuffW2     = 1.0f;
    s.outW1       = 0.08f;
    s.outW2       = 0.08f;
    s.outRed      = CANDLE_RED_MIN * (float)dutyFromPercent(BRIGHTNESS_MAX_RED) / (float)MAX_DUTY;
    s.lastUpdate = now;
    ledcWrite(LED_PINS[UV_LED], 0);
}

void updateCandleMode() {
    CandleState&  s   = candleState;
    unsigned long now = millis();
    unsigned long dt  = now - s.lastUpdate;
    if (dt == 0) return;
    s.lastUpdate = now;
    float dtf = (float)dt;

    // ── Sub-mode scheduler ────────────────────────────────────────────────────
    if (now >= s.subModeEnd && s.xfadeEnd == 0) {
        // Start crossfade to next sub-mode
        s.nextSubMode = pickNextSubMode(s.subMode);
        s.xfadeEnd    = now + CANDLE_XFADE_MS;
        s.subModeEnd  = s.xfadeEnd + random(CANDLE_SUBMODE_MIN_MS, CANDLE_SUBMODE_MAX_MS);
    }
    if (s.xfadeEnd != 0 && now >= s.xfadeEnd) {
        s.subMode  = s.nextSubMode;
        s.xfadeEnd = 0;
    }

    // ── Advance Perlin time axes ──────────────────────────────────────────────
    float speed1, speed2;
    switch (s.subMode) {
        case FlameSubMode::CALM:
            speed1 = CANDLE_CALM_SPEED;    speed2 = CANDLE_CALM_SPEED2;    break;
        case FlameSubMode::FLICKER:
            speed1 = CANDLE_FLICKER_SPEED; speed2 = CANDLE_FLICKER_SPEED2; break;
        default:
            speed1 = CANDLE_WIND_SPEED;    speed2 = CANDLE_WIND_SPEED2;    break;
    }
    s.tFast  += speed1 * dtf;
    s.tFast2 += speed2 * dtf;
    s.tGust  += CANDLE_WIND_GUST_SPEED * dtf;
    s.tW1    += CANDLE_W1_SPEED * dtf;
    s.tW2    += CANDLE_W2_SPEED * dtf;

    // ── Sample noise ──────────────────────────────────────────────────────────
    // inoise8 clusters around 128 (~64–192 practical range); rescale to 0–255.
    auto sampleNoise = [](uint32_t t) -> uint8_t {
        int raw = (int)inoise8(t & 0xFFFF);
        raw = (raw - 64) * 255 / 128;
        return (uint8_t)constrain(raw, 0, 255);
    };

    uint8_t n1       = sampleNoise((uint32_t)s.tFast);
    uint8_t n2       = sampleNoise((uint32_t)s.tFast2);
    uint8_t gustVal  = sampleNoise((uint32_t)s.tGust);
    float   curLevel = noiseToLevel(s.subMode, n1, n2, gustVal);

    // Diagnostic log every 2 seconds
    static unsigned long lastCandleLog = 0;
    if (now - lastCandleLog >= 2000) {
        const char* modeName = (s.subMode == FlameSubMode::CALM)    ? "CALM"    :
                               (s.subMode == FlameSubMode::FLICKER) ? "FLICKER" : "WIND";
        float alpha = constrain(smoothingForMode(s.subMode) * dtf, 0.0f, 1.0f);
        Serial.printf("[CANDLE] mode=%-7s  dt=%3lu  n1=%3d  n2=%3d  gust=%3d  "
                      "level=%.3f  W1=%.3f  W2=%.3f  red=%.3f  alpha=%.3f\n",
                      modeName, dt, n1, n2, gustVal, curLevel,
                      s.outW1, s.outW2, s.outRed, alpha);
        lastCandleLog = now;
    }

    // During xfade, blend toward the next sub-mode's noise
    if (s.xfadeEnd != 0) {
        float xfadeT = 1.0f - (float)(s.xfadeEnd - now) / (float)CANDLE_XFADE_MS;
        xfadeT = constrain(xfadeT, 0.0f, 1.0f);
        float nextLevel = noiseToLevel(s.nextSubMode,
            sampleNoise((uint32_t)(s.tFast + 7919)),
            sampleNoise((uint32_t)(s.tFast2 + 5003)),
            gustVal);
        curLevel = curLevel + (nextLevel - curLevel) * xfadeT;
    }

    // ── W1/W2 independent drift ───────────────────────────────────────────────
    // Each channel has its own slow Perlin axis, so they drift autonomously.
    // In calm mode the drift is wide; in flicker/wind it is narrow.
    float splitDepth = (s.subMode == FlameSubMode::CALM)
                       ? CANDLE_SPLIT_DEPTH
                       : CANDLE_SPLIT_DEPTH_FLICKER;
    float w1Noise = sampleNoise((uint32_t)s.tW1) / 255.0f;  // 0–1
    float w2Noise = sampleNoise((uint32_t)s.tW2) / 255.0f;
    // Each channel offsets from curLevel in its own direction
    float targetW1 = curLevel * (1.0f + splitDepth * (w1Noise - 0.5f) * 2.0f);
    float targetW2 = curLevel * (1.0f + splitDepth * (w2Noise - 0.5f) * 2.0f);
    targetW1 = constrain(targetW1, 0.0f, 1.0f);
    targetW2 = constrain(targetW2, 0.0f, 1.0f);

    // ── Snuff events (flicker mode only) ──────────────────────────────────────
    // A rare random trigger dips one channel toward zero then releases it.
    // The recovery uses its own faster smoothing so the return is snappy.
    if (s.subMode == FlameSubMode::FLICKER) {
        if (s.snuffW1 < 1.0f || random(0, 1000) < CANDLE_SNUFF_CHANCE) {
            if (s.snuffW1 >= 1.0f) s.snuffW1 = 1.0f - CANDLE_SNUFF_DEPTH;  // trigger dip
            s.snuffW1 += (1.0f - s.snuffW1) * constrain(CANDLE_SNUFF_RECOVER * dtf, 0.0f, 1.0f);
        }
        if (s.snuffW2 < 1.0f || random(0, 1000) < CANDLE_SNUFF_CHANCE) {
            if (s.snuffW2 >= 1.0f) s.snuffW2 = 1.0f - CANDLE_SNUFF_DEPTH;
            s.snuffW2 += (1.0f - s.snuffW2) * constrain(CANDLE_SNUFF_RECOVER * dtf, 0.0f, 1.0f);
        }
    } else {
        s.snuffW1 = 1.0f;
        s.snuffW2 = 1.0f;
    }
    targetW1 *= s.snuffW1;
    targetW2 *= s.snuffW2;

    // ── Red: inverse curve over its own full brightness range ─────────────────
    // dimness=0 when flame is at peak → red at RED_MIN fraction of its cap.
    // dimness=1 when flame is near zero → red at RED_MAX fraction of its cap.
    // Expressed as a fraction of BRIGHTNESS_MAX_RED so the full LED range is used.
    float peakLevel = (s.subMode == FlameSubMode::CALM)    ? CANDLE_CALM_BRIGHTNESS    :
                      (s.subMode == FlameSubMode::FLICKER) ? CANDLE_FLICKER_BRIGHTNESS :
                                                              CANDLE_WIND_BRIGHTNESS;
    float dimness    = 1.0f - constrain(curLevel / peakLevel, 0.0f, 1.0f);
    float redOfCap   = CANDLE_RED_MIN +
                       (CANDLE_RED_MAX - CANDLE_RED_MIN) * powf(dimness, CANDLE_RED_CURVE);
    // Convert fraction-of-cap to fraction-of-MAX_DUTY
    float targetRed  = redOfCap * (float)dutyFromPercent(BRIGHTNESS_MAX_RED) / (float)MAX_DUTY;

    // ── Exponential smoothing ─────────────────────────────────────────────────
    float smoothing = smoothingForMode(s.subMode);
    float alpha = constrain(smoothing * dtf, 0.0f, 1.0f);
    s.outW1  += (targetW1  - s.outW1)  * alpha;
    s.outW2  += (targetW2  - s.outW2)  * alpha;
    s.outRed += (targetRed - s.outRed) * alpha;

    // ── Write to LEDs ─────────────────────────────────────────────────────────
    ledcWrite(LED_PINS[WHITE_LED_1], (uint32_t)constrain((int)(s.outW1  * MAX_DUTY), 0, MAX_DUTY));
    ledcWrite(LED_PINS[WHITE_LED_2], (uint32_t)constrain((int)(s.outW2  * MAX_DUTY), 0, MAX_DUTY));
    ledcWrite(LED_PINS[RED_LED],     (uint32_t)constrain((int)(s.outRed * MAX_DUTY), 0, MAX_DUTY));
    ledcWrite(LED_PINS[UV_LED], 0);
}

void exitCandleMode() {
    // Turn off all LEDs
    for (int i = 0; i < 4; i++) {
        ledcWrite(LED_PINS[i], 0);
    }
}

// ── Color Mode ────────────────────────────────────────────────────────────────
//
// Hue advances at a speed that does a slow random walk between nearly-static
// and fast-cycling. The history buffer creates a spatial spread across LEDs.

void enterColorMode() {
    for (int i = 0; i < 4; i++) ledcWrite(LED_PINS[i], 0);

    colorHue         = (float)random(0, 256);
    colorCycleSpeed  = 0.015f;
    colorSpeedVel    = 0.0f;
    lastColorUpdate  = millis();
    lastHistoryPush  = millis();
    colorHistoryIndex = 0;
    for (int i = 0; i < COLOR_HISTORY_SIZE; i++)
        colorHistory[i] = (uint8_t)colorHue;
}

void updateColorMode() {
    unsigned long now = millis();
    unsigned long dt  = now - lastColorUpdate;
    if (dt < 20) return;
    lastColorUpdate = now;
    float dtf = (float)dt;

    // ── Speed random walk ─────────────────────────────────────────────────────
    // Velocity drifts randomly; soft walls pull speed back toward centre.
    float speedCentre = (COLOR_SPEED_MIN + COLOR_SPEED_MAX) * 0.5f;
    colorSpeedVel += (((float)random(0, 1000) / 500.0f) - 1.0f) * 0.000003f * dtf;
    colorSpeedVel *= 0.97f;
    colorSpeedVel += (speedCentre - colorCycleSpeed) * 0.000008f * dtf;
    colorCycleSpeed += colorSpeedVel * dtf;
    colorCycleSpeed = constrain(colorCycleSpeed, COLOR_SPEED_MIN, COLOR_SPEED_MAX);

    // ── Advance hue ───────────────────────────────────────────────────────────
    colorHue += colorCycleSpeed * dtf;
    if (colorHue >= 256.0f) colorHue -= 256.0f;

    // Push a new hue into the history ring at a rate proportional to speed,
    // so spatial spread across the strip scales with how fast things are moving.
    unsigned long histInterval = (unsigned long)(150.0f / (colorCycleSpeed / 0.015f));
    histInterval = constrain(histInterval, 30UL, 2000UL);
    if (now - lastHistoryPush > histInterval) {
        colorHistory[colorHistoryIndex] = (uint8_t)colorHue;
        colorHistoryIndex = (colorHistoryIndex + 1) % COLOR_HISTORY_SIZE;
        lastHistoryPush = now;
    }

    // ── Apply to LEDs ─────────────────────────────────────────────────────────
    for (int i = 0; i < NUM_LEDS; i++) {
        int pos = (colorHistoryIndex - i + COLOR_HISTORY_SIZE) % COLOR_HISTORY_SIZE;
        uint8_t h = colorHistory[pos] + (uint8_t)random(0, 4);
        leds[i] = CHSV(h, COLOR_SATURATION, COLOR_BRIGHTNESS);
    }

    // Spatial blur — softens boundaries between history steps
    for (int i = 0; i < NUM_LEDS; i++) {
        int prev = (i - 1 + NUM_LEDS) % NUM_LEDS;
        int next = (i + 1) % NUM_LEDS;
        CRGB c = leds[i]; c.nscale8(179);
        CRGB p = leds[prev]; p.nscale8(38);
        CRGB n = leds[next]; n.nscale8(38);
        leds[i] = c + p + n;
    }
}

void exitColorMode() {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

// ── Magic Mode ────────────────────────────────────────────────────────────────
//
// Two alternating phases:
//   DRIFT  – slow hue sweep across all LEDs, low brightness, atmospheric.
//   SPARK  – dim base with random bright sparks flying across the strip.
//
// Red LED drifts independently via a slow sine walk throughout.

static void magicStartPhase(MagicState& m, unsigned long now) {
    if (m.phase == MagicPhase::DRIFT) {
        m.phase    = MagicPhase::SPARK;
        m.phaseEnd = now + random(MAGIC_SPARK_PHASE_MIN, MAGIC_SPARK_PHASE_MAX);
    } else {
        m.phase      = MagicPhase::DRIFT;
        m.phaseEnd   = now + random(MAGIC_DRIFT_PHASE_MIN, MAGIC_DRIFT_PHASE_MAX);
        m.driftSpeed = MAGIC_DRIFT_SPEED_MIN +
                       ((float)random(0, 1000) / 1000.0f) *
                       (MAGIC_DRIFT_SPEED_MAX - MAGIC_DRIFT_SPEED_MIN);
    }
}

void enterMagicMode() {
    ledcWrite(LED_PINS[WHITE_LED_1], 0);
    ledcWrite(LED_PINS[WHITE_LED_2], 0);

    MagicState& m = magicState;
    unsigned long now = millis();
    m.phase      = MagicPhase::DRIFT;
    m.phaseEnd   = now + random(MAGIC_DRIFT_PHASE_MIN, MAGIC_DRIFT_PHASE_MAX);
    m.driftHue   = MAGIC_HUE_CENTER + (float)random(0, (int)MAGIC_HUE_SPREAD);
    m.driftSpeed = (MAGIC_DRIFT_SPEED_MIN + MAGIC_DRIFT_SPEED_MAX) * 0.5f;
    m.redLevel   = 0.4f;
    m.redVel     = 0.0f;
    m.lastUpdate = now;

    fill_solid(leds, NUM_LEDS, CRGB::Black);
    ledcWrite(LED_PINS[UV_LED], dutyFromPercent(BRIGHTNESS_MAX_UV));
}

void updateMagicMode() {
    MagicState&   m   = magicState;
    unsigned long now = millis();
    unsigned long dt  = now - m.lastUpdate;
    if (dt == 0) return;
    m.lastUpdate = now;
    float dtf = (float)dt;

    // ── Phase transitions ─────────────────────────────────────────────────────
    if (now >= m.phaseEnd) magicStartPhase(m, now);

    // ── Red LED — slow sine drift independent of phase ────────────────────────
    // Very slow drift — step is small, damping is heavy, centre pull is gentle.
    // Full range traversal takes on the order of minutes, not seconds.
    m.redVel += (((float)random(0, 1000) / 500.0f) - 1.0f) * MAGIC_RED_STEP * dtf;
    m.redVel *= MAGIC_RED_DAMPING;
    m.redVel += (MAGIC_RED_CENTRE - m.redLevel) * MAGIC_RED_PULL * dtf;
    m.redLevel += m.redVel * dtf;
    m.redLevel = constrain(m.redLevel, MAGIC_RED_MIN, MAGIC_RED_MAX);
    ledcWrite(LED_PINS[RED_LED],
        (uint32_t)(m.redLevel * (float)dutyFromPercent(BRIGHTNESS_MAX_RED)));

    // ── RGB strip ─────────────────────────────────────────────────────────────
    if (m.phase == MagicPhase::DRIFT) {
        // Advance hue slowly
        m.driftHue += m.driftSpeed * dtf;
        if (m.driftHue >= 256.0f) m.driftHue -= 256.0f;

        // All LEDs get similar hue with a gentle per-position sine ripple
        for (int i = 0; i < NUM_LEDS; i++) {
            float ripple = sinf((float)i * 0.45f + m.driftHue * 0.025f) * 6.0f;
            uint8_t h = (uint8_t)(m.driftHue + ripple);
            uint8_t v = MAGIC_DRIFT_BRIGHTNESS +
                        (uint8_t)(sinf((float)i * 0.3f + m.driftHue * 0.018f) * MAGIC_DRIFT_RIPPLE);
            leds[i] = CHSV(h, 230, v);
        }

    } else {
        // SPARK phase — dim base fades down, occasional sparks shoot along strip
        // Fade all LEDs toward a very dim base colour
        for (int i = 0; i < NUM_LEDS; i++) {
            leds[i].nscale8(MAGIC_SPARK_FADE);
            CRGB tint = CHSV((uint8_t)m.driftHue, 220, MAGIC_SPARK_TINT_V);
            leds[i] += tint;
        }

        if (random(0, 100) < MAGIC_SPARK_CHANCE) {
            int pos = random(0, NUM_LEDS);
            uint8_t sparkHue = (uint8_t)(m.driftHue + random(0, MAGIC_SPARK_HUE_SPREAD * 2) - MAGIC_SPARK_HUE_SPREAD);
            leds[pos] = CHSV(sparkHue, 200,
                MAGIC_SPARK_BRIGHTNESS_MIN + random(0, MAGIC_SPARK_BRIGHTNESS_MAX - MAGIC_SPARK_BRIGHTNESS_MIN));
        }

        // Advance driftHue slowly even in spark phase so colours evolve
        m.driftHue += 0.004f * dtf;
        if (m.driftHue >= 256.0f) m.driftHue -= 256.0f;
    }
}

void exitMagicMode() {
    for (int i = 0; i < 4; i++) ledcWrite(LED_PINS[i], 0);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

// Auto Mode Functions
void enterAutoMode() {
    currentAutoMode = CANDLE_MODE;
    lastAutoModeChange = millis();

    if (MODES[currentAutoMode].enterFunction) {
        MODES[currentAutoMode].enterFunction();
    }

    Serial.printf("[AUTO] Started - first sub-mode: %s\n", MODES[currentAutoMode].name);
}

void updateAutoMode() {
    unsigned long currentTime = millis();
    
    // Random mode change interval: 30 seconds to 3 minutes (30000-180000 ms)
    static unsigned long nextModeChangeInterval = random(30000, 180001);
    
    // Check if it's time to change modes
    if (currentTime - lastAutoModeChange > nextModeChangeInterval) {
        // Exit current auto mode
        if (MODES[currentAutoMode].exitFunction) {
            MODES[currentAutoMode].exitFunction();
        }
        
        // Select next random mode (excluding AUTO_MODE itself)
        CandleMode availableModes[] = {CANDLE_MODE, COLOR_MODE, MAGIC_MODE};
        int randomIndex = random(0, 3);
        currentAutoMode = availableModes[randomIndex];
        
        // Enter new auto mode
        if (MODES[currentAutoMode].enterFunction) {
            MODES[currentAutoMode].enterFunction();
        }
        
        lastAutoModeChange = currentTime;

        // Set new random interval for next mode change
        nextModeChangeInterval = random(30000, 180001);

        Serial.printf("[AUTO] -> %s  (next change in ~%lus)\n",
            MODES[currentAutoMode].name, nextModeChangeInterval / 1000);
    }
    
    // Update the current auto mode
    if (MODES[currentAutoMode].updateFunction) {
        MODES[currentAutoMode].updateFunction();
    }
}

void exitAutoMode() {
    // Exit the current auto mode
    if (MODES[currentAutoMode].exitFunction) {
        MODES[currentAutoMode].exitFunction();
    }
}
