#include <Arduino.h>

// Define LED pins
const int ledPins[4] = {15, 16, 17, 18}; // GPIO 17 listed twice, using 18 for the fourth LED

// PWM channel and frequency settings
const int pwmFreq = 5000;      // 5 kHz
const int pwmResolution = 8;   // 8-bit resolution

void setup() {
    pinMode(0, INPUT_PULLUP);
    for (int i = 0; i < 4; ++i) {
        ledcAttach(ledPins[i], pwmFreq, pwmResolution);
        ledcWrite(ledPins[i], 0);
    }
}

const int buttonPin = 0; // GPIO0 for user button
int currentLed = 0;
bool lastButtonState = HIGH;

void loop() {
    const int maxDuty = (1 << pwmResolution) - 1;
    const int maxBrightnessDuty = (maxDuty * 15) / 100; // 30%

    // Read button state (assuming pull-up, LOW when pressed)
    bool buttonState = digitalRead(buttonPin);

    // Detect button press (falling edge)
    if (lastButtonState == HIGH && buttonState == LOW) {
        // Turn off all LEDs
        for (int i = 0; i < 4; ++i) {
            ledcWrite(ledPins[i], 0);
        }
        // Move to next LED
        currentLed = (currentLed + 1) % 4;
        // Turn on current LED at 50% duty cycle
        ledcWrite(ledPins[currentLed], maxBrightnessDuty);
        delay(200); // Simple debounce
    }
    lastButtonState = buttonState;
}
