#include <Arduino.h>

const int pwmPins[] = {15, 16, 17, 18};
const int buttonPin = 0;
const int numLights = sizeof(pwmPins) / sizeof(pwmPins[0]);
int currentLight = 0;

void setup() {
    // Initialize PWM pins
    for (int i = 0; i < numLights; i++) {
        ledcSetup(i, 5000, 8); // 5 kHz frequency, 8-bit resolution
        ledcAttachPin(pwmPins[i], i);
        ledcWrite(i, 128); // 50% duty cycle
    }

    // Initialize button pin
    pinMode(buttonPin, INPUT_PULLUP);
}

void loop() {
    static bool lastButtonState = HIGH;
    bool buttonState = digitalRead(buttonPin);

    // Check for button press
    if (lastButtonState == HIGH && buttonState == LOW) {
        // Switch to the next light
        ledcWrite(currentLight, 0); // Turn off current light
        currentLight = (currentLight + 1) % numLights; // Move to the next light
        ledcWrite(currentLight, 128); // Turn on the next light
        delay(200); // Debounce delay
    }

    lastButtonState = buttonState;
}