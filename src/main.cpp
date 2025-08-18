#include <Arduino.h>
#include <FastLED.h>

// Pin definitions
const int LED_PINS[4] = {15, 16, 17, 18}; // White, Red, UV, UV LEDs
const int WS2812_PIN = 33;
const int BUTTON_PIN = 0;

// PWM settings
const double PWM_FREQ = 5000.0;
const uint8_t PWM_RESOLUTION = 8;
const uint8_t PWM_CHANNELS[4] = {0, 1, 2, 3}; // LEDC channels for each LED
const int MAX_DUTY = (1 << PWM_RESOLUTION) - 1;
const int MAX_BRIGHTNESS = (MAX_DUTY * 30) / 100; // 30% max brightness

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
int flickerBrightness[2] = {0, 0}; // For white and red LEDs
int colorHue = 0;
int colorRotation = 0;
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
        ledcSetup(PWM_CHANNELS[i], 5000.0, 8);
        ledcAttachPin(LED_PINS[i], PWM_CHANNELS[i]);
        ledcWrite(PWM_CHANNELS[i], 0);
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
    // Turn off PWM LEDs
    for (int i = 0; i < 4; i++) {
        ledcWrite(PWM_CHANNELS[i], 0);
    }
    
    // Turn off WS2812 LEDs
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

void setPWMBrightness(int ledIndex, int brightness) {
    brightness = constrain(brightness, 0, MAX_BRIGHTNESS);
    ledcWrite(PWM_CHANNELS[ledIndex], brightness);
}

// Candle Mode Functions
void enterCandleMode() {
    // Initialize flicker brightness for white and red LEDs
    flickerBrightness[0] = random(MAX_BRIGHTNESS / 5, MAX_BRIGHTNESS);
    flickerBrightness[1] = random(MAX_BRIGHTNESS / 5, MAX_BRIGHTNESS);
    lastFlickerUpdate = millis();
}

void updateCandleMode() {
    unsigned long currentTime = millis();
    
    if (currentTime - lastFlickerUpdate > random(50, 200)) { // Random flicker timing
        // Update white LED (index 0)
        int targetBrightness = random(MAX_BRIGHTNESS / 5, MAX_BRIGHTNESS);
        flickerBrightness[0] = lerp8by8(flickerBrightness[0], targetBrightness, 128);
        setPWMBrightness(0, flickerBrightness[0]);
        
        // Update red LED (index 1)
        targetBrightness = random(MAX_BRIGHTNESS / 5, MAX_BRIGHTNESS);
        flickerBrightness[1] = lerp8by8(flickerBrightness[1], targetBrightness, 128);
        setPWMBrightness(1, flickerBrightness[1]);
        
        lastFlickerUpdate = currentTime;
    }
}

void exitCandleMode() {
    // Turn off white and red LEDs
    ledcWrite(PWM_CHANNELS[0], 0);
    ledcWrite(PWM_CHANNELS[1], 0);
}

// Color Mode Functions
void enterColorMode() {
    colorHue = 0;
    colorRotation = 0;
    lastColorUpdate = millis();
}

void updateColorMode() {
    unsigned long currentTime = millis();
    
    if (currentTime - lastColorUpdate > 150) { // Slower update for smooth rotation
        // Create 3-4 random color mixtures that slowly rotate
        const int numColors = 4;
        int baseHues[numColors];
        
        // Generate random base hues with good spacing
        for (int i = 0; i < numColors; i++) {
            baseHues[i] = (colorHue + (i * 64) + random(0, 32)) % 256;
        }
        
        // Apply colors to LEDs with smooth rotation
        for (int i = 0; i < NUM_LEDS; i++) {
            int colorIndex = ((i + colorRotation) % NUM_LEDS) * numColors / NUM_LEDS;
            int hue = baseHues[colorIndex];
            
            // Add some variation to saturation and value for more natural look
            uint8_t sat = 200 + random(0, 55); // 200-255 saturation
            uint8_t val = 180 + random(0, 75); // 180-255 value
            
            leds[i] = CHSV(hue, sat, val);
        }
        
        // Slow rotation - only move every few updates
        if (currentTime % 3000 < 150) { // Every 3 seconds
            colorRotation = (colorRotation + 1) % NUM_LEDS;
        }
        
        // Very slow hue shift
        if (currentTime % 10000 < 150) { // Every 10 seconds
            colorHue = (colorHue + 8) % 256;
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
    magicHue = 96; // Start with green
    magicDirection = true;
    lastMagicUpdate = millis();
    
    // Turn on UV LEDs (indices 2 and 3)
    setPWMBrightness(2, MAX_BRIGHTNESS);
    setPWMBrightness(3, MAX_BRIGHTNESS);
}

void updateMagicMode() {
    unsigned long currentTime = millis();
    
    if (currentTime - lastMagicUpdate > 100) { // 10 FPS color transition
        // Update WS2812 LEDs with green-purple pulse
        uint8_t brightness = sin8(magicHue) * 255 / 256; // Create pulse effect
        
        for (int i = 0; i < NUM_LEDS; i++) {
            if (magicDirection) {
                // Green to purple
                leds[i] = CHSV(96 + (magicHue / 4), 255, brightness);
            } else {
                // Purple to green
                leds[i] = CHSV(192 - (magicHue / 4), 255, brightness);
            }
        }
        
        magicHue += 4;
        if (magicHue >= 255) {
            magicHue = 0;
            magicDirection = !magicDirection; // Switch direction
        }
        
        lastMagicUpdate = currentTime;
    }
}

void exitMagicMode() {
    // Turn off UV LEDs
    ledcWrite(PWM_CHANNELS[2], 0);
    ledcWrite(PWM_CHANNELS[3], 0);
    
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
    
    // Check if it's time to change modes (every 60 seconds)
    if (currentTime - lastAutoModeChange > 60000) {
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
