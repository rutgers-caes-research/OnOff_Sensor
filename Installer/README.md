# Installer development

This directory is reserved for the automatic Windows installer.

The intended installer will:

1. Detect the connected ESP32 chip and hardware identity.
2. Select the matching XIAO ESP32-S3 or XIAO ESP32-C3 release image.
3. Flash and verify the image.
4. Report the installed firmware version and device identifier.
5. Preserve sensor data and configuration by default.
6. Provide a separate, explicit factory-reset operation.

Release binaries will be generated from tagged source with pinned tool versions rather than committed Arduino build caches.

