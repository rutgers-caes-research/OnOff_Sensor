# Manual installation with Arduino IDE

## Requirements

- A Seeed Studio XIAO ESP32-S3 or XIAO ESP32-C3 sensor
- A USB data cable
- Arduino IDE 2.x
- `esp32 by Espressif Systems` board package version 3.3.10

The current V6 source uses libraries supplied by the ESP32 board package and does not require a separate MQTT library.

## Upload

1. Download or clone this repository.
2. Open `Firmware/Current/new_xiao_onoff_v6/new_xiao_onoff_v6.ino` in Arduino IDE.
3. Connect one sensor by USB.
4. Under **Tools > Board**, select the connected XIAO ESP32-S3 or XIAO ESP32-C3.
5. Under **Tools > Port**, select its COM port.
6. Select **Verify** to compile the firmware.
7. Select **Upload**.
8. After the board restarts, wait several seconds and connect to the `OnOff` configuration network.

The firmware detects board-specific behavior at compile time from the board selected in Arduino IDE. Selecting the wrong board can compile or upload the wrong image, so verify the board selection for every sensor.

## Field configuration

Use the sensor's web portal to set the Sensor ID, measurement timing, vibration threshold, power source, and optional network or MQTT settings. Download the current CSV before deleting it for reuse.

## Historical firmware

Files under `Firmware/Archive` are provided for code comparison only. Use `Firmware/Current` for new installations.
