# OnOff Sensor V5

`new_xiao_onoff_v5.ino` is one maintained source that compiles for both Seeed XIAO ESP32-S3 and ESP32-C3.

## Preserved behavior

- Existing three-column CSV contract: timestamp, numeric status, RMS value.
- V2 cold-start MPU correction: 100 ms stabilization plus 20 discarded readings.
- Four deployment choices: external USB battery with or without Wi-Fi recovery, and power cube with or without Wi-Fi recovery.
- Simple field portal with Start, Stop, Download CSV, Delete CSV, and configuration.

## V5 power behavior

- Both USB-bank modes: Wi-Fi normally off, 80 MHz wait baseline, and a validated one-second Wi-Fi keep-alive pulse every 25 seconds.
- S3 power-cube modes: deep sleep between measurements with BOOT-button wake.
- C3 power-cube modes: light sleep between measurements so the C3 BOOT button can wake the portal; the C3 BOOT GPIO cannot be used as a deep-sleep wake pin.
- The C3 has no controllable onboard user LED, so V5 does not claim LED status on that board.

Keep-alive timing is intentionally a developer constant rather than a student-facing setting.
