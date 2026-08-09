# Zigbee2MQTT custom converter

This folder contains a custom converter for the Espressif blind controller exposed in this project.

## Files

- `custom-esp32-blind-cover.js` — custom Zigbee2MQTT converter

## Install

Copy the file into your Zigbee2MQTT custom converter directory and restart Zigbee2MQTT.

Typical locations:

- Docker: `/app/data/custom-converters/`
- Home Assistant add-on: `/config/zigbee2mqtt/custom-converters/`
- Standalone Linux: `/etc/zigbee2mqtt/custom-converters/`

## Attribute

The firmware exposes this manufacturer-specific attribute on the Window Covering cluster:

- attribute ID: `0xF010`
- type: `uint32`
- unit: milliseconds
- meaning: full travel time for a 0–100% move

Example values:

- `2000`
- `4000`
- `6000`

## Usage

After the converter loads, the device should expose a numeric setting named `travel_time_ms` that can be set from Zigbee2MQTT or Home Assistant.

The firmware uses the value to calculate the required motor run time:

`move_time = abs(target - current) / 100 * travel_time_ms`
