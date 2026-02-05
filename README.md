# Terrarium Lid Controller

Firmware for an **ESP32-C3 Super Mini** terrarium light controller with:

- DS3231 RTC time-based light scheduling
- Potentiometer-controlled brightness
- MOSFET PWM output driving a 3W LED
- USB serial console commands (including RTC date/time query)

## Repository layout

- `TerrariumLidController/TerrariumLidController.ino` – main control loop and hardware behavior
- `TerrariumLidController/ConsoleInterface.h/.cpp` – extensible USB serial command interface
- `TerrariumLidController/sketch.yaml` – Arduino CLI profile with required platform/library metadata

## Arduino CLI setup

The compile error `Platform 'esp32:esp32' not found` means the ESP32 platform core is not installed locally.

Install prerequisites:

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "RTClib"
```

## Build

From the repo root:

```bash
arduino-cli compile --profile esp32c3 TerrariumLidController
```

Or explicitly with FQBN:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3 TerrariumLidController
```

## Upload (example)

```bash
arduino-cli upload -p <serial-port> --fqbn esp32:esp32:esp32c3 TerrariumLidController
```

## USB Console commands

Open serial monitor at **115200 baud** and press enter to get the prompt.

- `help` – show available commands
- `now` (aliases: `time`, `datetime`) – read and print DS3231 date/time
