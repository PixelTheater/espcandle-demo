#include <Arduino.h>
#include <FastLED.h>

// Pin definitions
const int LED_PINS[4] = {15, 16, 17, 18}; // White, White, UV, Deep Red LEDs
const int WHITE_LED_1 = 0;    // GPIO15 - Bright White LED 1
const int WHITE_LED_2 = 1;    // GPIO16 - Bright White LED 2  
const int UV_LED = 2;         // GPIO17 - UV LED
const int RED_LED = 3;        // GPIO18 - Deep Red LED
const int WS2812_PIN = 33;
const int BUTTON_PIN = 0;

// PWM settings
const double PWM_FREQ = 5000.0;
const uint8_t PWM_RESOLUTION = 8;
const int MAX_DUTY = (1 << PWM_RESOLUTION) - 1;
const int MAX_BRIGHTNESS = (MAX_DUTY * 15) / 100; // 30% max brightness

// WS2812 settings
const int NUM_LEDS = 20; // 20 colored LEDs
CRGB leds[NUM_LEDS];

// Button handling
const unsigned long LONG_PRESS_TIME = 3000; // 3 seconds
bool lastButtonState = HIGH;
unsigned long buttonPressStart = 0;
bool buttonPressed = false;

// Mode management
enum CandleMode {
    CANDLE_MODE,
    COLOR_MODE,
    MAGIC_MODE,
    AUTO_MODE,
    NUM_MODES
};

CandleMode currentMode = CANDLE_MODE;
CandleMode lastActiveMode = CANDLE_MODE;
bool powerOn = true;

// Mode-specific variables
unsigned long lastFlickerUpdate = 0;
unsigned long lastColorUpdate = 0;
unsigned long lastMagicUpdate = 0;
unsigned long lastAutoModeChange = 0;
int flickerBrightness[4] = {0, 0, 0, 0}; // For all 4 LEDs
int targetBrightness[4] = {0, 0, 0, 0};  // Target brightness for smooth transitions
unsigned long lastCandleDisturbance = 0;
bool candleIsCalm = true;
int calmBaseBrightness[4] = {0, 0, 0, 0}; // Base brightness during calm periods

// Color mode variables for smooth transitions
int currentColorHue = 0;           // Current base hue (0-255)
unsigned long colorModeStartTime = 0;
const int COLOR_HISTORY_SIZE = NUM_LEDS + 5; // Extra buffer for smooth blending
uint8_t colorHistory[COLOR_HISTORY_SIZE];   // Circular buffer of hues
int colorHistoryIndex = 0;         // Current position in history buffer
int magicHue = 0;
bool magicDirection = true; // true = green to purple, false = purple to green
CandleMode currentAutoMode = CANDLE_MODE;

// Mode structure
struct ModeConfig {
    const char* name;
    void (*updateFunction)();
    void (*enterFunction)();
    void (*exitFunction)();
};

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
const ModeConfig MODES[NUM_MODES] = {
    {"Candle", updateCandleMode, enterCandleMode, exitCandleMode},
    {"Color", updateColorMode, enterColorMode, exitColorMode},
    {"Magic", updateMagicMode, enterMagicMode, exitMagicMode},
    {"Auto", updateAutoMode, enterAutoMode, exitAutoMode}
};

void setup() {
    // Initialize button
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Initialize PWM LEDs
    for (int i = 0; i < 4; i++) {
        ledcAttach(LED_PINS[i], 5000.0, 8);
        ledcWrite(LED_PINS[i], 0);
    }
    
    // Initialize WS2812 LEDs
    FastLED.addLeds<WS2812, WS2812_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(64); // 25% brightness for WS2812
    
    // Enter initial mode
    if (MODES[currentMode].enterFunction) {
        MODES[currentMode].enterFunction();
    }
    
    Serial.begin(115200);
    Serial.println("ESP32 Candle initialized");
}

void loop() {
    handleButton();
    
    if (powerOn && MODES[currentMode].updateFunction) {
        MODES[currentMode].updateFunction();
    }
    
    FastLED.show(); // Update WS2812 LEDs
    delay(20); // Small delay for stability
}

void handleButton() {
    bool buttonState = digitalRead(BUTTON_PIN);
    
    // Button press detection
    if (lastButtonState == HIGH && buttonState == LOW) {
        buttonPressStart = millis();
        buttonPressed = true;
    }
    
    // Button release detection
    if (lastButtonState == LOW && buttonState == HIGH && buttonPressed) {
        unsigned long pressDuration = millis() - buttonPressStart;
        buttonPressed = false;
        
        if (pressDuration < LONG_PRESS_TIME) {
            // Short press - mode change or power on
            if (powerOn) {
                // Exit current mode
                if (MODES[currentMode].exitFunction) {
                    MODES[currentMode].exitFunction();
                }
                
                // Switch to next mode
                currentMode = (CandleMode)((currentMode + 1) % NUM_MODES);
                lastActiveMode = currentMode;
                
                // Enter new mode
                if (MODES[currentMode].enterFunction) {
                    MODES[currentMode].enterFunction();
                }
                
                Serial.print("Switched to mode: ");
                Serial.println(MODES[currentMode].name);
            } else {
                // Power on and restore last mode
                powerOn = true;
                currentMode = lastActiveMode;
                if (MODES[currentMode].enterFunction) {
                    MODES[currentMode].enterFunction();
                }
                Serial.println("Power on");
            }
        } else {
            // Long press - power off
            powerOn = false;
            turnOffAllLEDs();
            Serial.println("Power off");
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
    brightness = constrain(brightness, 0, MAX_BRIGHTNESS);
    ledcWrite(LED_PINS[ledIndex], brightness);
}

// Candle Mode Functions
void enterCandleMode() {
    // Initialize all LEDs to off first
    for (int i = 0; i < 4; i++) {
        flickerBrightness[i] = 0;
        targetBrightness[i] = 0;
        calmBaseBrightness[i] = 0;
    }
    
    // Initialize calm base brightness levels for candle LEDs only
    calmBaseBrightness[WHITE_LED_1] = (MAX_BRIGHTNESS * 75) / 100; // 75% of max
    calmBaseBrightness[WHITE_LED_2] = (MAX_BRIGHTNESS * 72) / 100; // 72% of max (slight variation)
    calmBaseBrightness[RED_LED] = (MAX_BRIGHTNESS * 35) / 100;     // 35% of max
    // UV_LED stays at 0 (off in candle mode)
    
    // Start with calm state for candle LEDs
    flickerBrightness[WHITE_LED_1] = calmBaseBrightness[WHITE_LED_1];
    flickerBrightness[WHITE_LED_2] = calmBaseBrightness[WHITE_LED_2];
    flickerBrightness[RED_LED] = calmBaseBrightness[RED_LED];
    targetBrightness[WHITE_LED_1] = calmBaseBrightness[WHITE_LED_1];
    targetBrightness[WHITE_LED_2] = calmBaseBrightness[WHITE_LED_2];
    targetBrightness[RED_LED] = calmBaseBrightness[RED_LED];
    
    candleIsCalm = true;
    lastFlickerUpdate = millis();
    lastCandleDisturbance = millis();
}

void updateCandleMode() {
    unsigned long currentTime = millis();
    
    // Check for disturbance events (every 3-8 seconds during calm periods)
    if (candleIsCalm && (currentTime - lastCandleDisturbance > random(3000, 8000))) {
        candleIsCalm = false;
        lastCandleDisturbance = currentTime;
    }
    
    // Return to calm after 500-1500ms of disturbance
    if (!candleIsCalm && (currentTime - lastCandleDisturbance > random(500, 1500))) {
        candleIsCalm = true;
        lastCandleDisturbance = currentTime;
    }
    
    // Update at 60Hz for smooth transitions
    if (currentTime - lastFlickerUpdate > 16) { // ~60 FPS
        
        // Update candle LEDs: WHITE_LED_1, WHITE_LED_2, RED_LED (but NOT UV_LED)
        int candleLEDs[] = {WHITE_LED_1, WHITE_LED_2, RED_LED};
        for (int i = 0; i < 3; i++) {
            int led = candleLEDs[i];
            
            if (candleIsCalm) {
                // Calm period: very gentle variation around base brightness
                int variation = random(-8, 9); // ±8 brightness units
                targetBrightness[led] = calmBaseBrightness[led] + variation;
                targetBrightness[led] = constrain(targetBrightness[led], 
                                                calmBaseBrightness[led] - 15, 
                                                calmBaseBrightness[led] + 15);
            } else {
                // Disturbance period: more noticeable but still controlled flicker
                int baseLevel = calmBaseBrightness[led];
                int flickerRange = baseLevel / 3; // Allow 33% variation from base
                targetBrightness[led] = random(baseLevel - flickerRange, baseLevel + flickerRange + 1);
            }
            
            // Smooth transition to target (slower during calm, faster during disturbance)
            int transitionSpeed = candleIsCalm ? 32 : 64; // Slower = more gradual
            flickerBrightness[led] = lerp8by8(flickerBrightness[led], targetBrightness[led], transitionSpeed);
            
            // Apply brightness
            setPWMBrightness(led, flickerBrightness[led]);
        }
        
        // Ensure UV LED is off in candle mode
        setPWMBrightness(UV_LED, 0);
        
        lastFlickerUpdate = currentTime;
    }
}

void exitCandleMode() {
    // Turn off all LEDs
    for (int i = 0; i < 4; i++) {
        ledcWrite(LED_PINS[i], 0);
    }
}

// Color Mode Functions
void enterColorMode() {
    // Turn off all PWM LEDs (only use WS2812 in color mode)
    for (int i = 0; i < 4; i++) {
        ledcWrite(LED_PINS[i], 0);
    }
    
    // Start with a random color each time
    currentColorHue = random(0, 256);
    colorModeStartTime = millis();
    lastColorUpdate = millis();
    colorHistoryIndex = 0;
    
    // Initialize color history buffer with random starting hue
    for (int i = 0; i < COLOR_HISTORY_SIZE; i++) {
        colorHistory[i] = currentColorHue; // Start with random color
    }
}

void updateColorMode() {
    unsigned long currentTime = millis();
    
    // Update every 50ms for smooth transitions (20 FPS)
    if (currentTime - lastColorUpdate > 50) {
        // Calculate progression through rainbow over 60 seconds
        unsigned long elapsedTime = currentTime - colorModeStartTime;
        float rainbowProgress = (elapsedTime % 120000) / 120000.0; // 0.0 to 1.0 over 120 seconds
        currentColorHue = (int)(rainbowProgress * 255); // 0 to 255 hue range
        
        // Add new hue to history buffer every ~150ms (3 seconds / 20 LEDs)
        static unsigned long lastHistoryUpdate = 0;
        if (currentTime - lastHistoryUpdate > 150) {
            colorHistory[colorHistoryIndex] = currentColorHue;
            colorHistoryIndex = (colorHistoryIndex + 1) % COLOR_HISTORY_SIZE;
            lastHistoryUpdate = currentTime;
        }
        
        // Apply colors to LEDs with history-based spacing
        for (int i = 0; i < NUM_LEDS; i++) {
            // Calculate which history position this LED should use
            int historyOffset = i; // Each LED is one step behind the previous
            int historyPos = (colorHistoryIndex - historyOffset + COLOR_HISTORY_SIZE) % COLOR_HISTORY_SIZE;
            uint8_t ledHue = colorHistory[historyPos];
            
            // Add slight hue variation for more natural look
            ledHue += random(-3, 4); // ±3 hue variation
            
            // Set LED with full saturation and good brightness
            leds[i] = CHSV(ledHue, 255, 200);
        }
        
        // Apply blur effect to smooth transitions (especially at wraparound)
        // Blend each LED with its neighbors
        for (int i = 0; i < NUM_LEDS; i++) {
            int prevLED = (i - 1 + NUM_LEDS) % NUM_LEDS;
            int nextLED = (i + 1) % NUM_LEDS;
            
            // Blend current LED with 20% of each neighbor
            CRGB blended = leds[i];
            blended.nscale8(179); // 70% of original (179/255 ≈ 0.7)
            
            CRGB prevColor = leds[prevLED];
            prevColor.nscale8(38); // 15% of neighbor (38/255 ≈ 0.15)
            
            CRGB nextColor = leds[nextLED];
            nextColor.nscale8(38); // 15% of neighbor
            
            leds[i] = blended + prevColor + nextColor;
        }
        
        lastColorUpdate = currentTime;
    }
}

void exitColorMode() {
    // Turn off WS2812 LEDs
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

// Magic Mode Functions
void enterMagicMode() {
    // Turn off white LEDs in magic mode
    ledcWrite(LED_PINS[WHITE_LED_1], 0);
    ledcWrite(LED_PINS[WHITE_LED_2], 0);
    
    magicHue = 0; // Start at beginning of color range
    magicDirection = true;
    lastMagicUpdate = millis();
    
    // Turn on UV LED at MAXIMUM brightness (ignore MAX_BRIGHTNESS limit for UV)
    ledcWrite(LED_PINS[UV_LED], 150); // Full 8-bit PWM for maximum UV output
    
    // Turn on deep red LED at reduced brightness (30% less than normal max)
    int reducedBrightness = (MAX_BRIGHTNESS * 7) / 10; // 70% of max
    setPWMBrightness(RED_LED, reducedBrightness);
}

void updateMagicMode() {
    unsigned long currentTime = millis();
    
    // Update at 30 FPS for much smoother transitions
    if (currentTime - lastMagicUpdate > 33) { // ~30 FPS instead of 10 FPS
        // Calculate smooth progression over time instead of discrete steps
        unsigned long elapsedTime = currentTime - lastMagicUpdate;
        static float smoothHue = 0.0;
        
        // Smooth continuous hue progression (slower than before)
        smoothHue += 0.5; // Much smaller increment for smoother transitions
        if (smoothHue >= 255.0) {
            smoothHue = 0.0;
            magicDirection = !magicDirection; // Switch direction
        }
        
        // Create smoother brightness pulse using floating point
        float pulsePhase = smoothHue * 2.0 * PI / 255.0; // Convert to radians
        uint8_t baseBrightness = (uint8_t)((sin(pulsePhase) * 0.5 + 0.5) * 150); // Smooth sine wave 0-150
        
        for (int i = 0; i < NUM_LEDS; i++) {
            float hueFloat;
            if (magicDirection) {
                // Deep purple to dark blue-green (hue range: 192 to 128)
                hueFloat = 192.0 - (smoothHue * 64.0 / 255.0);
            } else {
                // Dark blue-green to deep purple (hue range: 128 to 192)  
                hueFloat = 128.0 + (smoothHue * 64.0 / 255.0);
            }
            
            // Add subtle per-LED variation for more organic look
            float ledHueVariation = sin((float)i * 0.3 + smoothHue * 0.02) * 2.0;
            uint8_t finalHue = (uint8_t)(hueFloat + ledHueVariation);
            
            // Slight brightness variation per LED
            uint8_t ledBrightness = baseBrightness + (uint8_t)(sin((float)i * 0.5 + smoothHue * 0.03) * 10);
            
            leds[i] = CHSV(finalHue, 255, ledBrightness);
        }
        
        lastMagicUpdate = currentTime;
    }
}

void exitMagicMode() {
    // Turn off all PWM LEDs
    for (int i = 0; i < 4; i++) {
        ledcWrite(LED_PINS[i], 0);
    }
    
    // Turn off WS2812 LEDs
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

// Auto Mode Functions
void enterAutoMode() {
    currentAutoMode = CANDLE_MODE;
    lastAutoModeChange = millis();
    
    // Enter the first auto mode
    if (MODES[currentAutoMode].enterFunction) {
        MODES[currentAutoMode].enterFunction();
    }
    
    Serial.println("Auto mode started - Candle");
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
        
        Serial.print("Auto mode switched to: ");
        Serial.println(MODES[currentAutoMode].name);
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
