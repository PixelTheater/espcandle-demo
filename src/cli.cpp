#include "cli.h"
#include "config.h"
#include "types.h"

// ─── Extern references to main.cpp globals ───────────────────────────────────

extern CRGB leds[];
extern CandleMode currentMode;
extern bool powerOn;
extern const ModeConfig MODES[];

void turnOffAllLEDs();

// ─── Channel mask ─────────────────────────────────────────────────────────────
// Bits 0-3 = PWM channels (indices match LED_PINS).
// Bit 4 = virtual RGB channel (WS2812 strip).

static constexpr uint8_t CH_W1  = 1 << WHITE_LED_1; // 0x01
static constexpr uint8_t CH_W2  = 1 << WHITE_LED_2; // 0x02
static constexpr uint8_t CH_UV  = 1 << UV_LED;      // 0x04
static constexpr uint8_t CH_RED = 1 << RED_LED;     // 0x08
static constexpr uint8_t CH_RGB = 0x10;             // WS2812 strip
static constexpr uint8_t CH_PWM = CH_W1 | CH_W2 | CH_UV | CH_RED;
static constexpr uint8_t CH_ALL = CH_PWM | CH_RGB;

static uint8_t parseChannelMask(const char* token) {
    if (!token || token[0] == '\0') return CH_ALL;

    uint8_t mask = 0;
    char tmp[32];
    strncpy(tmp, token, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char* sp = nullptr;
    char* part = strtok_r(tmp, ",", &sp);
    while (part) {
        for (char* p = part; *p; p++) *p = tolower((unsigned char)*p);

        if      (strcmp(part, "w1")    == 0) mask |= CH_W1;
        else if (strcmp(part, "w2")    == 0) mask |= CH_W2;
        else if (strcmp(part, "white") == 0) mask |= CH_W1 | CH_W2;
        else if (strcmp(part, "uv")    == 0) mask |= CH_UV;
        else if (strcmp(part, "red")   == 0) mask |= CH_RED;
        else if (strcmp(part, "rgb")   == 0) mask |= CH_RGB;
        else if (strcmp(part, "all")   == 0) mask |= CH_ALL;
        else {
            Serial.printf("[CLI] Unknown channel '%s'  (w1 w2 white uv red rgb all)\n", part);
            return CH_ALL;
        }
        part = strtok_r(nullptr, ",", &sp);
    }
    return mask ? mask : CH_ALL;
}

static void printMaskName(uint8_t mask) {
    bool first = true;
    auto pr = [&](const char* n) {
        if (!first) Serial.print(',');
        Serial.print(n);
        first = false;
    };
    if ((mask & (CH_W1 | CH_W2)) == (CH_W1 | CH_W2)) pr("white");
    else {
        if (mask & CH_W1)  pr("w1");
        if (mask & CH_W2)  pr("w2");
    }
    if (mask & CH_UV)  pr("uv");
    if (mask & CH_RED) pr("red");
    if (mask & CH_RGB) pr("rgb");
}

// ─── CLI state ────────────────────────────────────────────────────────────────

enum class CliContext { NORMAL, TEST };

static CliContext ctx       = CliContext::NORMAL;
static bool      testActive = false;

// Per-channel last-duty memory for `on` command (PWM channels 0-3)
static int  savedDuty[4]    = {0, 0, 0, 0};
static CRGB savedRgbColor   = CRGB::White;

// Test parameters (shared across test commands)
static int     testMinBrightness = 0;
static int     testMaxBrightness = MAX_DUTY;
static double  testPwmFreq       = PWM_FREQ;
static uint8_t testChannelMask   = CH_ALL;

// Current solid RGB colour for `hold`/`on` support on the RGB channel
static CRGB currentRgbColor = CRGB::White;

// ─── Input / history ─────────────────────────────────────────────────────────

static constexpr int BUF_SIZE     = 64;
static constexpr int HISTORY_SIZE = 8;

static char    inputBuf[BUF_SIZE];
static uint8_t inputLen = 0;

static char history[HISTORY_SIZE][BUF_SIZE];
static int  historyCount = 0;
static int  historyPos   = -1;

enum class EscState { NONE, ESC, BRACKET };
static EscState escState = EscState::NONE;

static void historyPush(const char* line) {
    if (historyCount > 0 &&
        strcmp(history[(historyCount - 1) % HISTORY_SIZE], line) == 0) return;
    strncpy(history[historyCount % HISTORY_SIZE], line, BUF_SIZE - 1);
    history[historyCount % HISTORY_SIZE][BUF_SIZE - 1] = '\0';
    historyCount++;
}

static void replaceInputLine(const char* newLine) {
    Serial.print("\r\033[K");
    strncpy(inputBuf, newLine, BUF_SIZE - 1);
    inputBuf[BUF_SIZE - 1] = '\0';
    inputLen = (uint8_t)strlen(inputBuf);
    Serial.print(inputBuf);
}

// ─── PWM / LED helpers ────────────────────────────────────────────────────────

static void setChannelDuty(uint8_t mask, int duty) {
    duty = constrain(duty, 0, MAX_DUTY);
    if (mask & CH_W1)  { ledcWrite(LED_PINS[WHITE_LED_1], duty); savedDuty[WHITE_LED_1] = duty; }
    if (mask & CH_W2)  { ledcWrite(LED_PINS[WHITE_LED_2], duty); savedDuty[WHITE_LED_2] = duty; }
    if (mask & CH_UV)  { ledcWrite(LED_PINS[UV_LED],      duty); savedDuty[UV_LED]      = duty; }
    if (mask & CH_RED) { ledcWrite(LED_PINS[RED_LED],     duty); savedDuty[RED_LED]     = duty; }
    if (mask & CH_RGB) {
        fill_solid(leds, NUM_LEDS, currentRgbColor);
        // Scale brightness: duty/255 applied as FastLED global brightness for simplicity
        FastLED.setBrightness((uint8_t)duty);
        FastLED.show();
    }
}

static void setRgbColor(CRGB color) {
    currentRgbColor = color;
    savedRgbColor   = color;
    fill_solid(leds, NUM_LEDS, color);
    FastLED.show();
}

static void testSetPwmFreq(double freq) {
    for (int i = 0; i < 4; i++) {
        ledcAttach(LED_PINS[i], freq, PWM_RESOLUTION);
        ledcWrite(LED_PINS[i], 0);
    }
    testPwmFreq = freq;
}

static void cmdOff(uint8_t mask) {
    // savedDuty is maintained by setChannelDuty/testHold — just zero the outputs
    if (mask & CH_W1)  ledcWrite(LED_PINS[WHITE_LED_1], 0);
    if (mask & CH_W2)  ledcWrite(LED_PINS[WHITE_LED_2], 0);
    if (mask & CH_UV)  ledcWrite(LED_PINS[UV_LED],      0);
    if (mask & CH_RED) ledcWrite(LED_PINS[RED_LED],     0);
    if (mask & CH_RGB) {
        savedRgbColor = currentRgbColor;
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        FastLED.show();
    }
    Serial.printf("[OFF] ");
    printMaskName(mask);
    Serial.println();
}

static void cmdOn(uint8_t mask) {
    if (mask & CH_W1)  ledcWrite(LED_PINS[WHITE_LED_1], savedDuty[WHITE_LED_1]);
    if (mask & CH_W2)  ledcWrite(LED_PINS[WHITE_LED_2], savedDuty[WHITE_LED_2]);
    if (mask & CH_UV)  ledcWrite(LED_PINS[UV_LED],      savedDuty[UV_LED]);
    if (mask & CH_RED) ledcWrite(LED_PINS[RED_LED],     savedDuty[RED_LED]);
    if (mask & CH_RGB) {
        currentRgbColor = savedRgbColor;
        fill_solid(leds, NUM_LEDS, currentRgbColor);
        FastLED.show();
    }
    Serial.printf("[ON] ");
    printMaskName(mask);
    if (mask & CH_W1)  Serial.printf("  w1=%d",  savedDuty[WHITE_LED_1]);
    if (mask & CH_W2)  Serial.printf("  w2=%d",  savedDuty[WHITE_LED_2]);
    if (mask & CH_UV)  Serial.printf("  uv=%d",  savedDuty[UV_LED]);
    if (mask & CH_RED) Serial.printf("  red=%d", savedDuty[RED_LED]);
    if (mask & CH_RGB) Serial.printf("  #%02x%02x%02x",
        savedRgbColor.r, savedRgbColor.g, savedRgbColor.b);
    Serial.println();
}

static void testAllOff() {
    cmdOff(CH_ALL);
}

// ─── Color name / hex parser ─────────────────────────────────────────────────

static bool parseColor(const char* token, CRGB& out) {
    if (!token || token[0] == '\0') return false;

    // Hex: #rrggbb or rrggbb
    const char* hex = (token[0] == '#') ? token + 1 : token;
    if (strlen(hex) == 6) {
        bool allHex = true;
        for (int i = 0; i < 6; i++)
            if (!isxdigit((unsigned char)hex[i])) { allHex = false; break; }
        if (allHex) {
            unsigned long v = strtoul(hex, nullptr, 16);
            out = CRGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
            return true;
        }
    }

    // Named colours
    char tmp[20];
    strncpy(tmp, token, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp; *p; p++) *p = tolower((unsigned char)*p);

    struct { const char* name; CRGB color; } table[] = {
        {"white",   CRGB::White},
        {"red",     CRGB::Red},
        {"green",   CRGB::Green},
        {"blue",    CRGB::Blue},
        {"yellow",  CRGB::Yellow},
        {"cyan",    CRGB::Cyan},
        {"magenta", CRGB::Magenta},
        {"orange",  CRGB::Orange},
        {"purple",  CRGB::Purple},
        {"pink",    CRGB::DeepPink},
        {"warm",    CRGB(255, 147, 41)},  // warm white ~2700K
        {"black",   CRGB::Black},
    };
    for (auto& e : table) {
        if (strcmp(tmp, e.name) == 0) { out = e.color; return true; }
    }
    return false;
}

// ─── Menus ────────────────────────────────────────────────────────────────────

static void printNormalMenu() {
    Serial.println("\n--- ESP Candle CLI ---");
    Serial.println("  s          status");
    Serial.println("  m <0-3>    set mode  (0=Candle 1=Color 2=Magic 3=Auto)");
    Serial.println("  t          enter test mode");
    Serial.println("  ?          this menu");
    Serial.println("----------------------");
}

static void printTestMenu() {
    Serial.println("\n--- Test Mode ---");
    Serial.println("Channels: w1 w2 white uv red rgb all  (comma-separated)");
    Serial.printf("  min <0-%d> [ch]    set min brightness\n", MAX_DUTY);
    Serial.printf("  max <0-%d> [ch]    set max brightness\n", MAX_DUTY);
    Serial.println("  freq <hz>             set PWM freq (100-40000)");
    Serial.println("  info                  show current test params");
    Serial.println("  off [ch]              turn off channel(s)");
    Serial.println("  on  [ch]              restore last value for channel(s)");
    Serial.printf("  hold <0-%d> [ch]  hold PWM duty / RGB brightness\n", MAX_DUTY);
    Serial.println("  ramp [ch]             ramp min->max->min, printing each step");
    Serial.println("  rand [ch]             random bursts for 10 s");
    Serial.println("RGB:");
    Serial.println("  rgb <color|#rrggbb>   set solid color (white red green blue");
    Serial.println("                         yellow cyan magenta orange purple pink warm)");
    Serial.println("  rgb ramp              brightness ramp min->max->min on current color");
    Serial.println("  rgb rainbow           rainbow cycle for 10 s");
    Serial.println("  rgb chase             single pixel chase for 5 laps");
    Serial.println("  exit                  return to normal mode");
    Serial.println("-----------------");
}

// ─── Normal-mode commands ─────────────────────────────────────────────────────

static void cmdStatus() {
    Serial.printf("[STATUS] uptime=%lus  power=%s  mode=%s  heap=%dB  temp=%.1fC\n",
        millis() / 1000,
        powerOn ? "ON" : "OFF",
        MODES[currentMode].name,
        ESP.getFreeHeap(),
        temperatureRead());
}

static void cmdSetMode(int n) {
    if (n < 0 || n >= NUM_MODES) {
        Serial.printf("[CLI] Invalid mode %d (0-%d)\n", n, NUM_MODES - 1);
        return;
    }
    if (MODES[currentMode].exitFunction) MODES[currentMode].exitFunction();
    CandleMode prev = currentMode;
    currentMode = (CandleMode)n;
    if (MODES[currentMode].enterFunction) MODES[currentMode].enterFunction();
    powerOn = true;
    Serial.printf("[MODE] %s -> %s\n", MODES[prev].name, MODES[currentMode].name);
}

// ─── Test-mode commands ───────────────────────────────────────────────────────

static void testInfo() {
    Serial.printf("[TEST] min=%d  max=%d  freq=%.0f Hz  ch=",
        testMinBrightness, testMaxBrightness, testPwmFreq);
    printMaskName(testChannelMask);
    Serial.printf("  rgb=#%02x%02x%02x\n",
        currentRgbColor.r, currentRgbColor.g, currentRgbColor.b);
}

// Ramp PWM channels in mask; for RGB channel ramps brightness on currentRgbColor
static void testRamp(uint8_t mask) {
    uint8_t pwmMask = mask & CH_PWM;
    bool    doRgb   = (mask & CH_RGB) != 0;

    Serial.printf("[RAMP] ");
    printMaskName(mask);
    Serial.printf("  %d->%d->%d  freq=%.0f Hz\n",
        testMinBrightness, testMaxBrightness, testMinBrightness, testPwmFreq);

    for (int v = testMinBrightness; v <= testMaxBrightness; v++) {
        if (pwmMask) setChannelDuty(pwmMask, v);
        if (doRgb) {
            fill_solid(leds, NUM_LEDS, currentRgbColor);
            FastLED.setBrightness((uint8_t)v);
            FastLED.show();
        }
        Serial.printf("  up   v=%3d\n", v);
        delay(8);
    }
    for (int v = testMaxBrightness; v >= testMinBrightness; v--) {
        if (pwmMask) setChannelDuty(pwmMask, v);
        if (doRgb) {
            fill_solid(leds, NUM_LEDS, currentRgbColor);
            FastLED.setBrightness((uint8_t)v);
            FastLED.show();
        }
        Serial.printf("  down v=%3d\n", v);
        delay(8);
    }
    if (pwmMask) setChannelDuty(pwmMask, 0);
    if (doRgb)  { fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.setBrightness(255); FastLED.show(); }
    Serial.println("[RAMP] done");
}

static void testRand(uint8_t mask) {
    uint8_t pwmMask = mask & CH_PWM;
    bool    doRgb   = (mask & CH_RGB) != 0;
    int range = testMaxBrightness - testMinBrightness;
    if (range < 1) range = 1;

    Serial.printf("[RAND] ");
    printMaskName(mask);
    Serial.printf("  10 s  range=%d-%d\n", testMinBrightness, testMaxBrightness);

    unsigned long end = millis() + 10000;
    while (millis() < end) {
        int v = testMinBrightness + random(0, range + 1);
        if (pwmMask) setChannelDuty(pwmMask, v);
        if (doRgb) {
            fill_solid(leds, NUM_LEDS, currentRgbColor);
            FastLED.setBrightness((uint8_t)v);
            FastLED.show();
        }
        Serial.printf("  v=%3d\n", v);
        delay(80);
    }
    if (pwmMask) setChannelDuty(pwmMask, 0);
    if (doRgb)  { fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.setBrightness(255); FastLED.show(); }
    Serial.println("[RAND] done");
}

static void testHold(int duty, uint8_t mask) {
    duty = constrain(duty, 0, MAX_DUTY);
    uint8_t pwmMask = mask & CH_PWM;
    bool    doRgb   = (mask & CH_RGB) != 0;

    if (pwmMask) {
        // Write directly via ledcWrite per channel and update savedDuty
        if (pwmMask & CH_W1)  { ledcWrite(LED_PINS[WHITE_LED_1], duty); savedDuty[WHITE_LED_1] = duty; }
        if (pwmMask & CH_W2)  { ledcWrite(LED_PINS[WHITE_LED_2], duty); savedDuty[WHITE_LED_2] = duty; }
        if (pwmMask & CH_UV)  { ledcWrite(LED_PINS[UV_LED],      duty); savedDuty[UV_LED]      = duty; }
        if (pwmMask & CH_RED) { ledcWrite(LED_PINS[RED_LED],     duty); savedDuty[RED_LED]     = duty; }
    }
    if (doRgb) {
        fill_solid(leds, NUM_LEDS, currentRgbColor);
        FastLED.setBrightness((uint8_t)duty);
        FastLED.show();
    }
    Serial.printf("[HOLD] duty=%d  ch=", duty);
    printMaskName(mask);
    // Confirm actual pin values written
    Serial.print("  pins:");
    if (pwmMask & CH_W1)  Serial.printf(" GPIO%d=%d", LED_PINS[WHITE_LED_1], duty);
    if (pwmMask & CH_W2)  Serial.printf(" GPIO%d=%d", LED_PINS[WHITE_LED_2], duty);
    if (pwmMask & CH_UV)  Serial.printf(" GPIO%d=%d", LED_PINS[UV_LED],      duty);
    if (pwmMask & CH_RED) Serial.printf(" GPIO%d=%d", LED_PINS[RED_LED],     duty);
    Serial.println();
}

static void testRgbRainbow() {
    Serial.println("[RGB RAINBOW] 10 s");
    unsigned long end = millis() + 10000;
    uint8_t hue = 0;
    FastLED.setBrightness((uint8_t)testMaxBrightness);
    while (millis() < end) {
        for (int i = 0; i < NUM_LEDS; i++) {
            leds[i] = CHSV((uint8_t)(hue + i * (255 / NUM_LEDS)), 255, 255);
        }
        FastLED.show();
        Serial.printf("  hue=%3d\n", hue);
        hue++;
        delay(30);
    }
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.setBrightness(255);
    FastLED.show();
    Serial.println("[RGB RAINBOW] done");
}

static void testRgbChase() {
    const int LAPS = 5;
    Serial.printf("[RGB CHASE] %d laps\n", LAPS);
    FastLED.setBrightness((uint8_t)testMaxBrightness);
    for (int lap = 0; lap < LAPS; lap++) {
        for (int i = 0; i < NUM_LEDS; i++) {
            fill_solid(leds, NUM_LEDS, CRGB::Black);
            leds[i] = CHSV((uint8_t)(lap * 51), 255, 255);
            FastLED.show();
            Serial.printf("  lap=%d  led=%2d\n", lap, i);
            delay(50);
        }
    }
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.setBrightness(255);
    FastLED.show();
    Serial.println("[RGB CHASE] done");
}

// ─── Command dispatchers ──────────────────────────────────────────────────────

static void dispatchNormal(char* line) {
    if      (strcmp(line, "s") == 0) cmdStatus();
    else if (strcmp(line, "t") == 0) {
        ctx        = CliContext::TEST;
        testActive = true;
        testAllOff();
        testSetPwmFreq(testPwmFreq);
        testInfo();
        printTestMenu();
    }
    else if (strncmp(line, "m ", 2) == 0) cmdSetMode(atoi(line + 2));
    else if (strcmp(line, "?")  == 0) printNormalMenu();
    else Serial.printf("[CLI] Unknown: '%s'  (? for help)\n", line);
}

// Parse "value [channel]" — returns channel token ptr or nullptr
static const char* splitValueChan(const char* args, int* valueOut) {
    char* end = nullptr;
    *valueOut = (int)strtol(args, &end, 10);
    if (end == args) return nullptr;
    while (*end == ' ') end++;
    return (*end != '\0') ? end : nullptr;
}

static void dispatchTest(char* line) {

    // ── off [ch]  OR  <ch> off ────────────────────────────────────────────────
    if (strcmp(line, "off") == 0) {
        cmdOff(CH_ALL);

    } else if (strncmp(line, "off ", 4) == 0) {
        cmdOff(parseChannelMask(line + 4));

    // ── on [ch]  OR  <ch> on ──────────────────────────────────────────────────
    } else if (strcmp(line, "on") == 0) {
        cmdOn(CH_ALL);

    } else if (strncmp(line, "on ", 3) == 0) {
        cmdOn(parseChannelMask(line + 3));

    // ── min / max ─────────────────────────────────────────────────────────────
    } else if (strncmp(line, "min ", 4) == 0) {
        int v; const char* ch = splitValueChan(line + 4, &v);
        testMinBrightness = constrain(v, 0, MAX_DUTY);
        if (ch) testChannelMask = parseChannelMask(ch);
        Serial.printf("[TEST] min=%d  ch=", testMinBrightness);
        printMaskName(testChannelMask); Serial.println();

    } else if (strncmp(line, "max ", 4) == 0) {
        int v; const char* ch = splitValueChan(line + 4, &v);
        testMaxBrightness = constrain(v, 0, MAX_DUTY);
        if (ch) testChannelMask = parseChannelMask(ch);
        Serial.printf("[TEST] max=%d  ch=", testMaxBrightness);
        printMaskName(testChannelMask); Serial.println();

    // ── freq ──────────────────────────────────────────────────────────────────
    } else if (strncmp(line, "freq ", 5) == 0) {
        double f = atof(line + 5);
        if (f < 100 || f > 40000) Serial.println("[TEST] freq must be 100-40000 Hz");
        else { testSetPwmFreq(f); Serial.printf("[TEST] freq=%.0f Hz\n", testPwmFreq); }

    // ── info ──────────────────────────────────────────────────────────────────
    } else if (strcmp(line, "info") == 0) {
        testInfo();

    // ── hold <duty> [ch] ──────────────────────────────────────────────────────
    } else if (strncmp(line, "hold ", 5) == 0) {
        int v; const char* ch = splitValueChan(line + 5, &v);
        uint8_t mask = ch ? parseChannelMask(ch) : testChannelMask;
        testHold(v, mask);

    // ── ramp [ch] ─────────────────────────────────────────────────────────────
    } else if (strncmp(line, "ramp", 4) == 0) {
        const char* ch = (line[4] == ' ') ? line + 5 : nullptr;
        uint8_t mask = ch ? parseChannelMask(ch) : testChannelMask;
        testRamp(mask);

    // ── rand [ch] ─────────────────────────────────────────────────────────────
    } else if (strncmp(line, "rand", 4) == 0) {
        const char* ch = (line[4] == ' ') ? line + 5 : nullptr;
        uint8_t mask = ch ? parseChannelMask(ch) : testChannelMask;
        testRand(mask);

    // ── rgb ... ───────────────────────────────────────────────────────────────
    } else if (strncmp(line, "rgb", 3) == 0) {
        const char* arg = (line[3] == ' ') ? line + 4 : "";

        if (strcmp(arg, "ramp") == 0) {
            testRamp(CH_RGB);

        } else if (strcmp(arg, "rainbow") == 0) {
            testRgbRainbow();

        } else if (strcmp(arg, "chase") == 0) {
            testRgbChase();

        } else if (strcmp(arg, "off") == 0 || strcmp(arg, "") == 0) {
            cmdOff(CH_RGB);

        } else {
            CRGB color;
            if (parseColor(arg, color)) {
                setRgbColor(color);
                Serial.printf("[RGB] #%02x%02x%02x\n", color.r, color.g, color.b);
            } else {
                Serial.printf("[RGB] Unknown color '%s'\n", arg);
                Serial.println("  Colors: white red green blue yellow cyan magenta");
                Serial.println("          orange purple pink warm  or  #rrggbb");
            }
        }

    // ── exit ──────────────────────────────────────────────────────────────────
    } else if (strcmp(line, "exit") == 0) {
        testAllOff();
        for (int i = 0; i < 4; i++) {
            ledcAttach(LED_PINS[i], PWM_FREQ, PWM_RESOLUTION);
            ledcWrite(LED_PINS[i], 0);
        }
        FastLED.setBrightness(WS2812_BRIGHTNESS);
        testActive = false;
        ctx = CliContext::NORMAL;
        Serial.println("[TEST] Exiting test mode");
        printNormalMenu();

    } else if (strcmp(line, "?") == 0) {
        printTestMenu();

    } else {
        // ── Try reversed syntax: "<ch> on", "<ch> off", "<ch> hold <duty>" ──
        // Find the last space to split "token verb"
        char* sp = strrchr(line, ' ');
        bool handled = false;
        if (sp) {
            const char* verb = sp + 1;
            char chToken[32];
            size_t chLen = sp - line;
            if (chLen < sizeof(chToken)) {
                strncpy(chToken, line, chLen);
                chToken[chLen] = '\0';
                if (strcmp(verb, "on") == 0) {
                    cmdOn(parseChannelMask(chToken));
                    handled = true;
                } else if (strcmp(verb, "off") == 0) {
                    cmdOff(parseChannelMask(chToken));
                    handled = true;
                }
            }
            // "<ch> hold <duty>" — first token is channel, second is "hold", third is value
            // e.g. "uv hold 100"
            char* sp2 = strchr(line, ' ');
            if (!handled && sp2) {
                char ch2[16]; size_t n = sp2 - line;
                if (n < sizeof(ch2)) {
                    strncpy(ch2, line, n); ch2[n] = '\0';
                    const char* rest = sp2 + 1;
                    if (strncmp(rest, "hold ", 5) == 0) {
                        int v = atoi(rest + 5);
                        testHold(v, parseChannelMask(ch2));
                        handled = true;
                    }
                }
            }
        }
        if (!handled)
            Serial.printf("[TEST] Unknown: '%s'  (? for help)\n", line);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void cliBegin() {
    printNormalMenu();
}

void cliUpdate() {
    while (Serial.available()) {
        char c = (char)Serial.read();

        // ── ANSI escape sequence ───────────────────────────────────────────────
        if (escState == EscState::NONE && c == '\x1b') {
            escState = EscState::ESC;
            continue;
        }
        if (escState == EscState::ESC) {
            escState = (c == '[') ? EscState::BRACKET : EscState::NONE;
            continue;
        }
        if (escState == EscState::BRACKET) {
            escState = EscState::NONE;
            if (c == 'A') {
                if (historyCount == 0) continue;
                if (historyPos < 0) historyPos = historyCount - 1;
                else if (historyPos > 0) historyPos--;
                replaceInputLine(history[historyPos % HISTORY_SIZE]);
            } else if (c == 'B') {
                if (historyPos < 0) continue;
                historyPos++;
                if (historyPos >= historyCount) { historyPos = -1; replaceInputLine(""); }
                else replaceInputLine(history[historyPos % HISTORY_SIZE]);
            }
            continue;
        }

        // ── Backspace ──────────────────────────────────────────────────────────
        if (c == '\x7f' || c == '\x08') {
            if (inputLen > 0) { inputLen--; inputBuf[inputLen] = '\0'; Serial.print("\x08 \x08"); }
            continue;
        }

        if (c == '\r') continue;

        // ── Enter ──────────────────────────────────────────────────────────────
        if (c == '\n') {
            Serial.println();
            inputBuf[inputLen] = '\0';
            while (inputLen > 0 && inputBuf[inputLen - 1] == ' ') inputBuf[--inputLen] = '\0';
            if (inputLen > 0) {
                historyPush(inputBuf);
                historyPos = -1;
                if (ctx == CliContext::NORMAL) dispatchNormal(inputBuf);
                else                           dispatchTest(inputBuf);
            }
            inputLen = 0;
            continue;
        }

        // ── Printable character ────────────────────────────────────────────────
        if (inputLen < BUF_SIZE - 1) { inputBuf[inputLen++] = c; Serial.print(c); }
    }
}

bool cliTestActive() {
    return testActive;
}
