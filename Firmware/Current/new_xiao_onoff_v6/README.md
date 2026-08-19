# OnOff Sensor V6.1.2

`new_xiao_onoff_v6.ino` extends the validated dual-board V5 firmware with optional MQTT delivery. It compiles from one source for Seeed XIAO ESP32-S3 and ESP32-C3.

## Preserved V5 behavior

- The first three CSV columns remain timestamp, numeric status, and RMS value.
- Timestamps remain in `MM/dd/yyyy HH:mm:ss` form and status remains numeric `0` or `1`.
- The MPU cold-start correction remains 100 ms stabilization, 20 discarded readings at 5 ms spacing, and a zeroed first high-pass-filter sample.
- Failed MPU measurements are discarded and never written as false OFF records.
- The four external USB battery / power cube deployment modes remain unchanged.
- Download CSV and Delete Current CSV remain together in the student-facing portal.
- V5 USB-bank keep-alive constants remain 80 MHz, one second every 25 seconds, with a 1.6-second measurement guard.

## Deployment choices

The portal presents two short choices that produce six supported combinations:

1. Power source: `External USB Battery` or `Power Cube / Wall Adapter`
2. Network use: `No Network Wi-Fi`, `Wi-Fi for Time Recovery`, or `Wi-Fi for Time Recovery + MQTT`

USB-battery MQTT connects only while uploading. Wall-powered MQTT keeps the network connection available, but both variants upload only rows that were successfully written to the CSV. The USB-bank keep-alive AP remains a power function and is not network connectivity.

## MQTT server choices

MQTT is disabled by default. The web portal offers:

1. `Default`
2. `Custom`

The default server matches the EzAmp MQTT transport and authentication. Any MQTT configuration can use a general MQTT Topic Prefix. The firmware appends the versioned OnOff path and immutable hardware UID.

Custom-server mode allows the user to override:

- Broker host and port
- Username and password
- MQTT Topic Prefix

The MQTT Topic Prefix is the only user-controlled part of the topic. The firmware-controlled suffix is `onoff/v1/devices/<device-uid>/readings`. The Device UID is the lowercase factory Wi-Fi MAC without separators and cannot be edited. The readable Sensor ID and automatic Deployment ID travel in the payload rather than controlling routing.

V6 uses the same unencrypted MQTT/TCP transport as EzAmp on port 1883. It does not provide MQTT over TLS.

## MQTT operation and persistent catch-up

- Every successfully written CSV row becomes one MQTT message. A row is saved when status changes or when the configured maximum time between CSV rows is reached. Measurements skipped between those saved rows are not published.
- The CSV is the durable upload backlog. A small persistent byte cursor identifies the oldest row that has not been broker-acknowledged; no second measurement file duplicates the CSV.
- Uploads use MQTT QoS 1. The cursor advances only after the ESP MQTT client receives `MQTT_EVENT_PUBLISHED` for that row.
- A failed Wi-Fi connection, broker connection, publish, or acknowledgment leaves the cursor unchanged so a later cycle can retry from the CSV.
- After a failed upload, V6 waits one minute before retrying and schedules the retry after measurement capture, preventing an unavailable network from delaying every sample.
- Catch-up is bounded to eight rows per acquisition cycle so a large backlog cannot indefinitely starve local sensing.
- The cursor is checkpointed every ten acknowledged rows to reduce flash wear. An abrupt reset can therefore repeat up to nine already-acknowledged rows; QoS 1 is at-least-once delivery, so consumers should deduplicate by Device UID, Deployment ID, and epoch.
- The portal blocks MQTT destination changes while records are pending. Existing rows are never silently rerouted.
- Delete Current CSV also resets its upload cursor.
- USB-battery MQTT disconnects Wi-Fi after each bounded upload batch and retains the V5 bank keep-alive architecture.
- Wall-powered MQTT stays awake with Wi-Fi/MQTT available, but it still uploads only CSV rows.

## Board execution model

- S3: the MQTT worker is pinned to core 0 while the Arduino measurement loop remains on its normal core.
- C3: the C3 has one CPU core, so the MQTT worker runs cooperatively after measurement capture. The firmware waits for pending MQTT work before beginning another MPU capture.
- The RAM queue contains only a wake signal. It cannot lose measurement data because the actual pending rows remain in the CSV.
- MQTT operations and catch-up batch size are bounded so an unavailable server cannot stop local acquisition indefinitely.

## MQTT payload

The topic is:

```text
<topic-prefix>/onoff/v1/devices/<device-uid>/readings
```

The default is:

```text
emqx/onoff/v1/devices/e072a1f988c4/readings
```

The comma-separated payload is:

```text
epoch,status,rms,sensor_id,deployment_id,device_uid,firmware,board
```

The first three values still match the authoritative CSV record. Sensor ID is a readable deployment label; Device UID permanently identifies the board.

## Safe reuse and portal safeguards

- A small information button opens the recommended initial-deployment and safe-reuse flows inside the portal.
- Renaming is blocked while the current CSV contains data, preventing a previous file from becoming hidden in LittleFS.
- MQTT broker or prefix changes are blocked while unacknowledged rows remain.
- Delete Current CSV warns when it will cancel pending MQTT uploads.
- Prepare for New Deployment is available only after the current CSV and backlog are cleared. It advances a persistent Deployment ID while retaining the Device UID.
- Older CSV files left by prior firmware are detected and shown only as a recovery warning with exact Download and Delete actions.
- Downloaded filenames include Sensor ID, Device UID, and Deployment ID; the internal three-column CSV content is unchanged.
- The sampling and maximum-CSV-row interval controls display a live remaining-capacity estimate. It shows the duration with no status changes and the minimum duration if status changes at every sample. The calculation uses the board's current free LittleFS space, 32 bytes per row, and a 10% reserve.

The 32-byte assumption is conservative relative to the measured 29-byte rows in the retained C3 and S3 23-hour CSV files. With an almost-empty filesystem, one row per minute is approximately 28-31 days, one row per five minutes is approximately 141-154 days, and a status change every 10-second sample is approximately 4.7-5.1 days. Existing CSV files reduce the displayed remaining duration.

## Build verification

Validated build environment:

- Arduino ESP32 core 3.3.10
- ESP-IDF `esp-mqtt` client supplied by Arduino ESP32 core 3.3.10
- C3 FQBN: `esp32:esp32:XIAO_ESP32C3`
- S3 FQBN: `esp32:esp32:XIAO_ESP32S3`
- Single-job compilation with `--jobs 1`

V6.1.2 build results on 2026-08-17:

| Board | Flash | Application partition | Global RAM |
|---|---:|---:|---:|
| XIAO ESP32-C3 | 1,269,635 bytes | 96% of 1,310,720 | 62,784 bytes |
| XIAO ESP32-S3 | 1,174,344 bytes | 35% of 3,342,336 | 72,508 bytes |

## V6.0.0 prototype validation on 2026-08-11

- Both C3 and S3 builds were uploaded with flash verification and started with the MPU6050 detected at address `0x68`.
- Both boards produced normal CSV measurements and received MQTT QoS 1 acknowledgments through the configured default broker.
- MQTT messages were produced only for records written to the CSV; conservative skipped measurements produced no MQTT message.
- During a controlled C3 outage, the CSV retained an ON row and the following OFF row. After Wi-Fi was restored, MQTT delivered those two pending records in timestamp order before continuing with live records.
- With the one-minute MQTT retry backoff, the unavailable network did not cause a connection timeout on every measurement cycle; subsequent samples remained near the configured ten-second cadence.
- Duplicate delivery was observed after restart, consistent with the intentional ten-row cursor checkpoint and QoS 1 at-least-once semantics.

These results apply to the tagged `onoff-v6.0.0-prototype`. V6.1.0 changes the topic and payload contract; its current validation is recorded in `Documentation/V6_1_VALIDATION_REPORT_2026-08-17.md`.

## Required deployment validation

Compilation does not validate server ingestion or power behavior. Before releasing V6:

1. Confirm the portal identifies V6 and the correct board.
2. Confirm phone-time sync, Start, BOOT-stop, Download, and Delete.
3. Confirm the first MPU reading remains consistent with later idle readings.
4. Confirm the immutable Device UID, Deployment ID, general prefix, and fixed generated topic.
5. Confirm server payload parsing and QoS 1 broker acknowledgments.
6. Disable network access long enough to create multiple CSV rows, then restore it and confirm ordered catch-up.
7. Reset during a backlog test and confirm the saved cursor resumes without a missing CSV row.
8. Confirm pending-backlog warnings block broker/prefix changes and require explicit discard before deletion.
9. Confirm rename blocking, orphan-file recovery, unique downloaded filenames, and Prepare for New Deployment.
10. Test C3 sample timing while wall-powered MQTT is active.
11. Repeat standalone USB-bank continuity tests for the battery + MQTT mode.
