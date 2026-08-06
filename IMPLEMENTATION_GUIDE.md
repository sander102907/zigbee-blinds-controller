# ESP32-H2 Zigbee LED Controller - Implementation Summary

## Overview
This is a battery-friendly Zigbee LED controller firmware for the ESP32-H2 SuperMini that connects to Home Assistant Zigbee2MQTT. The firmware can receive on/off commands and control the built-in LED while using light sleep to minimize power consumption.

## Project Structure

```
zigbee-blinds-controller/
├── main/
│   ├── led_light_controller.c      # Main application code
│   ├── led_light_controller.h      # Configuration header
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── esp-zigbee-sdk/                 # Espressif Zigbee SDK (cloned)
├── sdkconfig.defaults              # Default configuration
├── CMakeLists.txt
├── partitions.csv                  # Partition table
└── build/                          # Build artifacts
    ├── zigbee_led_controller.bin   # Application binary
    └── bootloader/
        └── bootloader.bin
```

## Hardware Configuration

### ESP32-H2 SuperMini LED GPIO
- **LED GPIO**: GPIO 3 (active low - standard for ESP32-H2 SuperMini)
- **Active Level**: 0 (LED turns on when GPIO is low)

### Wireless
- **Zigbee Channel**: 11 (default for Zigbee)
- **Device Type**: End Device (ZED) - battery-friendly
- **Radio Mode**: Native IEEE 802.15.4

## Features

### 1. **Zigbee Connectivity**
- Joins a Zigbee network as an end device (battery-optimized)
- Supports ZHA (Zigbee Home Automation) profile
- Full integration with Zigbee2MQTT and Home Assistant

### 2. **LED Control**
- Responds to on/off cluster commands
- Supports:
  - Individual on/off commands
  - Attribute-based state management
  - State reporting back to the network

### 3. **Power Optimization**
- **Light Sleep**: CPU enters light sleep when idle
- **RX Off**: Radio disabled when not needed (except during commissioning)
- **Tickless Idle**: FreeRTOS tickless mode for maximum sleep time
- **Flash Power Down**: Flash memory powers down during sleep

### 4. **Network Configuration**
- **NVS Storage**: Network credentials saved in "zb_storage" partition
- **Auto-commissioning**: Network steering enabled for easy joining
- **Factory Reset**: Device resets to factory defaults on first start

## Building the Firmware

### Prerequisites
- ESP-IDF v5.4 installed and configured
- ESP32-H2 SuperMini board connected via USB

### Build Steps

1. **Set Target to ESP32-H2** (if not already done):
   ```bash
   cd /path/to/zigbee-blinds-controller
   . $IDF_PATH/export.sh  # Linux/macOS
   # or
   & $env:IDF_PATH/export.ps1  # PowerShell
   idf.py set-target esp32h2
   ```

2. **Build the Firmware**:
   ```bash
   idf.py build
   ```

3. **Monitor Build**:
   ```bash
   idf.py build -v  # Verbose output
   ```

## Flashing the Firmware

### Method 1: Using ESP-IDF Tools
```bash
# Flash the firmware
idf.py flash

# Monitor serial output
idf.py monitor

# Combined: Build, Flash, and Monitor
idf.py build flash monitor
```

### Method 2: Using esptool.py Directly
```bash
# Erase flash
esptool.py -p COM3 erase_flash

# Flash bootloader
esptool.py -p COM3 write_flash 0x0 build/bootloader/bootloader.bin

# Flash partition table
esptool.py -p COM3 write_flash 0x8000 build/partition_table/partition-table.bin

# Flash application
esptool.py -p COM3 write_flash 0x20000 build/zigbee_led_controller.bin
```

Replace `COM3` with your actual serial port.

## Operation Guide

### 1. **Initial Setup**
- Device boots and initializes Zigbee stack
- Enters network commissioning mode if factory new
- Waits for network steering commands

### 2. **Joining a Network**
- Put device in commissioning mode
- In Zigbee2MQTT, enable permit join for 180 seconds
- Device will automatically scan and join the network
- Upon successful join, the built-in LED will respond to on/off commands

### 3. **Controlling the LED**
- From Home Assistant: Go to Zigbee2MQTT > LED Controller > On/Off
- Toggle the switch to control the LED
- LED state is synchronized with the Zigbee state

## Firmware Details

### Main Application Code (`led_light_controller.c`)
- **LED Management**: GPIO initialization and state control
- **Zigbee Stack**: Integration with esp-zigbee-lib
- **Command Handling**: ZCL attribute-based on/off cluster handling
- **Power Management**: Light sleep configuration and callbacks

### Configuration Header (`led_light_controller.h`)
- Zigbee channel configuration
- Device endpoint definition
- Partition names and storage configuration
- Manufacturer/model identifiers

### Build Configuration (`sdkconfig.defaults`)
- Zigbee enabled (CONFIG_ZB_ENABLED=y)
- End device role (CONFIG_ZB_ZED=y)
- Light sleep enabled
- Power management enabled
- IEEE 802.15.4 peripherals configured

## Binary Sizes
- **Bootloader**: ~21 KB
- **Application**: ~547 KB (43% of app partition)
- **Total Flash Usage**: ~570 KB

## Next Steps

### 1. **Flash to Device**
   - Connect ESP32-H2 SuperMini via USB
   - Run `idf.py flash monitor`
   - Verify UART output shows successful startup

### 2. **Network Integration**
   - Configure Zigbee2MQTT to permit join
   - Device should join within 30 seconds
   - Check Zigbee2MQTT logs for confirmation

### 3. **Home Assistant Setup**
   - Device should appear in HA after Zigbee2MQTT picks it up
   - Create automation for LED control
   - Monitor battery voltage (if applicable)

### 4. **Optimization Options**
   - Adjust light sleep duration in `esp_pm_config_t`
   - Reduce log levels for additional power savings
   - Extend poll intervals for battery devices

## Troubleshooting

### Device Won't Join Network
1. Check if Zigbee2MQTT has permit join enabled
2. Verify device is in proper channel
3. Check UART output for commissioning errors

### LED Not Responding
1. Verify GPIO 3 is correct for your board variant
2. Check if LED polarity is correct (active low vs. active high)
3. Monitor Zigbee2MQTT for message delivery

### High Power Consumption
1. Verify light sleep is enabled in menuconfig
2. Check if RX is staying enabled unnecessarily
3. Monitor for excessive logging output

## Configuration Options

To modify settings, use:
```bash
idf.py menuconfig
```

Key sections:
- **Component config > Zigbee**: Network and device configuration
- **Component config > Power Management**: Light sleep parameters
- **Component config > Hardware Settings**: GPIO and peripheral config

## Power Consumption Estimates

Expected current draw (estimate):
- **Sleep**: <10 µA
- **Active Radio**: ~50 mA (during communication)
- **CPU Active**: ~15 mA
- **Average (battery use)**: 5-10 mA (with light sleep)

This enables battery operation for several months with typical battery sizes.

## References

- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32h2/)
- [ESP Zigbee SDK](https://github.com/espressif/esp-zigbee-sdk)
- [Zigbee Specification](https://zigbeealliance.org/)
- [ESP32-H2 Datasheet](https://www.espressif.com/en/products/socs/esp32-h2/)

## Support

For issues or questions:
1. Check the build output for specific errors
2. Review the UART monitor output during runtime
3. Check Zigbee2MQTT logs for network-level errors
4. Refer to ESP-IDF documentation for framework-level issues
