# ESP32-S3 PWM Lights Control

This project controls four PWM lights using an ESP32-S3 microcontroller. The lights are connected to GPIOs 15, 16, 17, and 18, and a button is used to switch between the lights. The PWM duty cycle is set to 50%.

## Project Structure

```
esp32s3-pwm-lights
├── src
│   └── main.cpp
├── platformio.ini
└── README.md
```

## Setup Instructions

1. **Install PlatformIO**: Make sure you have PlatformIO installed in your development environment.

2. **Clone the Repository**: Clone this repository to your local machine.

3. **Open the Project**: Open the project folder in PlatformIO.

4. **Build the Project**: Use the build command to compile the code.

5. **Upload to ESP32-S3**: Connect your ESP32-S3 board and upload the firmware.

## Usage

- The lights will be controlled by the button connected to GPIO0.
- Press the button to cycle through the lights connected to GPIOs 15, 16, 17, and 18.
- Each light will be turned on with a PWM duty cycle of 50%.

## License

This project is licensed under the MIT License.