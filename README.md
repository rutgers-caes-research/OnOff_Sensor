# OnOff Sensor Firmware

Firmware for the Rutgers CAES OnOff vibration logger using Seeed Studio XIAO ESP32-S3 and XIAO ESP32-C3 boards.

## Current firmware

`Firmware/Current/new_xiao_onoff_v6/new_xiao_onoff_v6.ino` is the maintained dual-board source. Version 6.1.2 adds optional MQTT delivery while preserving local CSV logging and the established field workflow.

The same source supports both boards. Select the correct XIAO board in Arduino IDE before compiling or uploading.

## Repository contents

- `Firmware/Current` - current firmware and technical notes
- `Firmware/Archive` - source-only V1 through V5 snapshots for inspection
- `INSTALL.md` - manual Arduino IDE installation procedure
- `Installer` - future automatic Windows installer and release tooling

The archive is retained for comparison and is not recommended for new deployments. These snapshots were imported from the internal development repository; this distribution repository does not reproduce every original development commit. Normal Git history begins with this curated distribution.

## Supported deployment power

- External USB battery
- External USB battery with configured network features
- Power cube or wall adapter
- Power cube or wall adapter with configured network features

Integrated batteries are intentionally outside the supported deployment scope.

## Releases

The planned installer will detect whether a connected board is an ESP32-S3 or ESP32-C3 and flash the matching verified image. Until that installer is released, use the Arduino IDE procedure in `INSTALL.md`.
