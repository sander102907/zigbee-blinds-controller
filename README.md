# Zigbee Blinds Controller

This project turns an ESP32-H2 into a battery-powered Zigbee end device that controls a motorized blind or tilt actuator through the Zigbee Window Covering cluster.

## What it does

- Runs on ESP-IDF 5.4 with the ESP-Zigbee SDK
- Exposes a Zigbee Window Covering endpoint to Zigbee2MQTT / Home Assistant
- Accepts tilt percentage updates and maps them to motor direction
- Supports manual motor control from physical buttons
- Reports battery percentage using an ADC-based battery driver
- Uses a small LED indicator and PWM-based motor control

## Hardware

The firmware is written for an ESP32-H2 SuperMini-style board and expects:

- A motor driver connected to the PWM outputs defined in [main/motor_driver.c](main/motor_driver.c)
- Two physical buttons on GPIO 10 and GPIO 11 for local/manual control
- A battery divider connected to the ADC input used by [main/battery_driver.c](main/battery_driver.c)
- An LED on GPIO 8 for status indication

## Build and flash

1. Install ESP-IDF v5.4 and activate the environment.
2. Set the target:
   ```bash
   idf.py set-target esp32h2
   ```
3. Build the firmware:
   ```bash
   idf.py build
   ```
4. Flash it to the device:
   ```bash
   idf.py -p COM3 flash monitor
   ```

## Zigbee behavior

After flashing and pairing the device to a Zigbee coordinator:

- Zigbee2MQTT should discover the device as a cover-like device
- Sending tilt/position updates from Zigbee2MQTT will drive the motor
- The device exposes a manufacturer-specific Window Covering attribute to tune the full travel time:
  - Attribute ID: `0xF010`
  - Type: `uint32`
  - Units: milliseconds for 0–100% travel
  - Example: `4000` means a full 0% → 100% move takes 4 seconds
- The firmware uses this value when converting a requested percentage delta into a motor run duration.

## Notes

- If the device was previously paired under an older profile, it may be helpful to rejoin it after flashing.
- The current firmware uses the Window Covering cluster rather than the earlier light profile.

## Project files

- [main/main.c](main/main.c) — Zigbee stack setup, endpoint registration, and motor command handling
- [main/light_driver.c](main/light_driver.c) — LED output and Zigbee connection status indication
- [main/motor_driver.c](main/motor_driver.c) — PWM motor driver
- [main/battery_driver.c](main/battery_driver.c) — ADC battery measurement
- [main/main.h](main/main.h) — project constants and configuration
