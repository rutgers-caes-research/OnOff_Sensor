#include <WiFi.h>
#include <Wire.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <driver/gpio.h>

// AP Mode Configuration
#define AP_SSID "OnOff"
#define AP_PASS "onoff123"
constexpr const char* FIRMWARE_VERSION = "5.0.0";

#define sampleCount 1000

// --- XIAO BOARD MAPPING ---
// Both boards use the same physical D-pin locations, but their GPIO numbers
// and built-in controls differ.
#if CONFIG_IDF_TARGET_ESP32C3
constexpr const char* BOARD_NAME = "XIAO ESP32-C3";
constexpr int I2C_SDA = D4;          // GPIO6
constexpr int I2C_SCL = D5;          // GPIO7
constexpr int BUTTON_PIN = D9;       // GPIO9, onboard BOOT button
constexpr int LED_PIN = -1;          // C3 has no software-controlled onboard LED
constexpr bool HAS_STATUS_LED = false;
constexpr bool CUBE_USES_LIGHT_SLEEP = true;
#else
constexpr const char* BOARD_NAME = "XIAO ESP32-S3";
constexpr int I2C_SDA = D4;          // GPIO5
constexpr int I2C_SCL = D5;          // GPIO6
constexpr int BUTTON_PIN = 0;        // onboard BOOT button
constexpr int LED_PIN = LED_BUILTIN; // GPIO21, active LOW
constexpr bool HAS_STATUS_LED = true;
constexpr bool CUBE_USES_LIGHT_SLEEP = false;
#endif

// Proven on Bank 1 with V4-style MPU sampling for a continuous 12-hour run.
constexpr uint32_t USB_KEEPALIVE_INTERVAL_MS = 25000;
constexpr uint32_t USB_KEEPALIVE_DURATION_MS = 1000;
constexpr uint32_t MEASUREMENT_GUARD_MS = 1600;
constexpr uint32_t USB_BASELINE_CPU_MHZ = 80;

// Time Configuration (EST/EDT for New Jersey)
const char* tzInfo = "EST5EDT,M3.2.0,M11.1.0";
const char* ntpServer = "pool.ntp.org";

// The four field-facing deployment choices are stored as one preference.
enum DeploymentMode {
  USB_BANK_NO_WIFI = 0,
  USB_BANK_WIFI = 1,
  POWER_CUBE_NO_WIFI = 2,
  POWER_CUBE_WIFI = 3
};

// --- Variables ---
Preferences preferences;
String sensorID;
String csvFilename;
int sleepSeconds;
int maxLoggedIntervalMins;
float RSSsafety;
bool isRunning = false;
bool exitPortal = false;
bool flashMounted = false;

int deploymentMode = USB_BANK_NO_WIFI;
String wifiSSID;
String wifiPass;

float instAccelXYZ[3];
int machineStatus = 0;
uint8_t sensorAddress = 0x68;
bool sensorAvailable = false;
uint32_t normalCpuFrequencyMhz = 240;
uint64_t nextUsbKeepAliveUs = 0;

// Retained across power-cube deep-sleep cycles.
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int prevStatus = -1;
RTC_DATA_ATTR bool timeSynced = false;
RTC_DATA_ATTR bool distressMode = false;
RTC_DATA_ATTR time_t lastLoggedEpoch = 0;

// Filter and sample tracking
float RC = 0.1;
float dT = 1.0;
static float xyzHPF[3][sampleCount];
static float Accel[3][sampleCount];

WebServer server(80);

bool isUsbBankMode() {
  return deploymentMode == USB_BANK_NO_WIFI || deploymentMode == USB_BANK_WIFI;
}

bool wifiRecoveryEnabled() {
  return deploymentMode == USB_BANK_WIFI || deploymentMode == POWER_CUBE_WIFI;
}

void turnWifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// --- Status Light Helpers ---
void setStatusLed(bool on) {
  if (!HAS_STATUS_LED) return;
  digitalWrite(LED_PIN, on ? LOW : HIGH);
}

void blinkLED(int times, int durationMs) {
  if (!HAS_STATUS_LED) return;
  for (int i = 0; i < times; i++) {
    setStatusLed(true);
    delay(durationMs);
    setStatusLed(false);
    delay(durationMs);
  }
}

// --- Smart Delay (Allows button to interrupt USB-bank waits) ---
void smartDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("\n[INTERRUPT] Button pressed! Halting sensor and rebooting...");
      preferences.putBool("running", false);
      for (int i = 0; i < 10; i++) {
        setStatusLed(true);
        delay(50);
        setStatusLed(false);
        delay(50);
      }
      ESP.restart();
    }
    delay(10);
  }
}

// --- Sensor Functions ---
bool gyro_init() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  sensorAvailable = false;

  for (uint8_t address = 0x68; address <= 0x69; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      sensorAddress = address;
      sensorAvailable = true;
      break;
    }
  }

  if (!sensorAvailable) {
    Serial.println("[SENSOR ERROR] No accelerometer found at 0x68 or 0x69.");
    return false;
  }

  Wire.beginTransmission(sensorAddress);
  Wire.write(0x6B);
  Wire.write(0x00); // Keep the MPU awake in all supported deployment modes.
  if (Wire.endTransmission(true) != 0) {
    sensorAvailable = false;
    Serial.println("[SENSOR ERROR] Accelerometer wake command failed.");
    return false;
  }

  Serial.printf("[SENSOR] Accelerometer found at 0x%02X.\n", sensorAddress);
  return true;
}

bool readAccelOnce() {
  if (!sensorAvailable) return false;

  Wire.beginTransmission(sensorAddress);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)sensorAddress, (uint8_t)6, true) != 6) return false;

  int16_t rawX = Wire.read() << 8 | Wire.read();
  int16_t rawY = Wire.read() << 8 | Wire.read();
  int16_t rawZ = Wire.read() << 8 | Wire.read();

  instAccelXYZ[0] = (float)rawX / 16384.0;
  instAccelXYZ[1] = (float)rawY / 16384.0;
  instAccelXYZ[2] = (float)rawZ / 16384.0;
  return true;
}

bool gyro_signals() {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (readAccelOnce()) return true;
    delay(5);
    if (attempt == 1 && !gyro_init()) break;
  }

  sensorAvailable = false;
  Serial.printf("[SENSOR ERROR] Accelerometer at 0x%02X stopped responding.\n", sensorAddress);
  return false;
}

// --- Web Server Functions (AP Mode) ---
void handleRoot() {
  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<style>body{font-family: Arial; text-align: center; margin-top: 30px; background-color: #f4f6f9; color: #333;}";
  html += ".card{max-width: 400px; margin: 0 auto; background: white; padding: 30px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.1);}";
  html += "button{color: white; padding: 15px 32px; font-size: 18px; border: none; border-radius: 8px; cursor: pointer; width: 100%; margin-top: 10px; transition: 0.3s;}";
  html += ".btn-start{background-color: #28a745;} .btn-start:hover:enabled{background-color: #218838;}";
  html += ".btn-stop{background-color: #dc3545;} .btn-stop:hover{background-color: #c82333;}";
  html += ".btn-dl{background-color: #007bff;} .btn-dl:hover{background-color: #0056b3;}";
  html += ".btn-clear{background-color: #ff9800;} .btn-clear:hover{background-color: #e68a00;}";
  html += "input, select{width: 100%; padding: 10px; margin: 8px 0; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box;}</style>";

  html += "<script>";
  html += "function toggleWiFiFields() {";
  html += "  var mode = document.getElementById('deployment_mode').value;";
  html += "  var wifiDiv = document.getElementById('wifi_settings');";
  html += "  if(mode == '1' || mode == '3') { wifiDiv.style.display = 'block'; } else { wifiDiv.style.display = 'none'; }";
  html += "}";
  html += "</script></head>";

  html += "<body><div class=\"card\"><h2>OnOff Sensor Portal</h2>";
  html += "<p><strong>Sensor ID:</strong> " + sensorID + "</p>";
  html += "<p style=\"font-size:12px; color:#666;\"><strong>Firmware:</strong> V" + String(FIRMWARE_VERSION) + " | " + String(BOARD_NAME) + "</p>";

  if (isRunning) {
    html += "<h3 style=\"color:#28a745;\">Status: RUNNING</h3>";
    html += "<p style=\"font-size:12px; color:#666;\">(Will resume sampling when portal closes)</p>";
  } else {
    html += "<h3 style=\"color:#dc3545;\">Status: IDLE</h3>";
    html += "<p id=\"sync-status\" style=\"color:#ff9800; font-weight:bold;\">Syncing backup time from phone...</p>";
  }

  if (!flashMounted) {
    html += "<div style=\"background-color:#ffcccc; padding:10px; border-radius:8px; margin-bottom:15px;\">";
    html += "<h3 style=\"color:#dc3545; margin:0;\">Memory Error</h3>";
    html += "<p style=\"color:#dc3545; font-size:14px; margin-top:5px;\">LittleFS mount failed. Please check Arduino Partition Scheme.</p></div>";
  } else {
    File file = LittleFS.open(csvFilename.c_str(), FILE_READ);
    if (file) {
      html += "<p><strong>Data file size:</strong> " + String(file.size() / 1024.0, 2) + " KB</p>";
      file.close();
    } else {
      html += "<p style=\"color:#ff9800; font-weight:bold;\">No data file found yet.</p>";
    }
  }

  if (!isRunning) {
    html += "<a href=\"/start\"><button id=\"btn-start\" class=\"btn-start\" disabled style=\"opacity:0.5;\">Start Data Collection</button></a>";
  } else {
    html += "<a href=\"/stop\"><button class=\"btn-stop\">Stop / Idle Mode</button></a>";
  }

  html += "<a href=\"/download\"><button class=\"btn-dl\">Download CSV</button></a>";

  if (!isRunning) {
    html += "<a href=\"/clear\" onclick=\"return confirm('Delete " + sensorID + ".csv? Download it first if it is needed.');\"><button class=\"btn-clear\">Delete Current CSV</button></a>";
  }

  html += "<hr style=\"margin: 25px 0;\"><h3 style=\"text-align:left;\">Sensor Configuration</h3>";
  html += "<form action=\"/save\" method=\"POST\">";
  html += "<div style=\"text-align: left;\">";
  html += "<label>Sensor ID (CSV Filename):</label><br><input type=\"text\" name=\"id\" value=\"" + sensorID + "\"><br>";
  html += "<label>Sampling Interval (Sec):</label><br><input type=\"number\" min=\"1\" name=\"sleep\" value=\"" + String(sleepSeconds) + "\"><br>";
  html += "<label>Maximum Logged Interval (Mins):</label><br><input type=\"number\" min=\"1\" name=\"max_log_int\" value=\"" + String(maxLoggedIntervalMins) + "\"><br>";

  html += "<label>Vibration Threshold (RMS):</label><br>";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px; margin-bottom:4px;\"><em>(Default: 0.028 | Lowest recorded: 0.0068. Check CSV baseline to avoid false ONs from background noise.)</em></p>";
  html += "<input type=\"number\" step=\"0.0001\" name=\"rss_thresh\" value=\"" + String(RSSsafety, 4) + "\"><br>";

  html += "<label>Deployment Mode:</label><br>";
  html += "<select id=\"deployment_mode\" name=\"deployment_mode\" onchange=\"toggleWiFiFields()\">";
  html += "<option value=\"0\" " + String(deploymentMode == USB_BANK_NO_WIFI ? "selected" : "") + ">External USB Battery - No Wi-Fi</option>";
  html += "<option value=\"1\" " + String(deploymentMode == USB_BANK_WIFI ? "selected" : "") + ">External USB Battery - Wi-Fi Enabled</option>";
  html += "<option value=\"2\" " + String(deploymentMode == POWER_CUBE_NO_WIFI ? "selected" : "") + ">Power Cube - No Wi-Fi</option>";
  html += "<option value=\"3\" " + String(deploymentMode == POWER_CUBE_WIFI ? "selected" : "") + ">Power Cube - Wi-Fi Enabled</option>";
  html += "</select><br>";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px;\"><em>External USB Battery modes automatically keep compatible power banks awake.</em></p>";

  String displayStyle = wifiRecoveryEnabled() ? "block" : "none";
  html += "<div id=\"wifi_settings\" style=\"display:" + displayStyle + "; background-color:#e9ecef; padding:10px; border-radius:5px; margin-top:10px;\">";
  html += "<p style=\"font-size:12px; margin-top:0;\"><strong>Note:</strong> Used only for automatically recovering time after power loss. If Wi-Fi fails, the sensor requires phone time.</p>";
  html += "<label>WiFi SSID:</label><input type=\"text\" name=\"wifi_ssid\" value=\"" + wifiSSID + "\">";
  html += "<label>WiFi Password:</label><input type=\"password\" name=\"wifi_pass\" value=\"" + wifiPass + "\">";
  html += "</div>";

  html += "</div>";
  html += "<button type=\"submit\" class=\"btn-dl\" style=\"background-color:#6c757d; margin-top:15px;\">Save Settings</button>";
  html += "</form>";
  html += "</div>";

  if (!isRunning) {
    html += "<script>";
    html += "window.onload = function() {";
    html += "  var ts = Math.floor(Date.now() / 1000);";
    html += "  fetch('/settime?ts=' + ts).then(function(response) {";
    html += "    if(response.ok) {";
    html += "      var st = document.getElementById('sync-status');";
    html += "      st.innerHTML = 'Time Synced to Phone';";
    html += "      st.style.color = '#28a745';";
    html += "      var btn = document.getElementById('btn-start');";
    html += "      if(btn) { btn.disabled = false; btn.style.opacity = 1; }";
    html += "    }";
    html += "  });";
    html += "};";
    html += "toggleWiFiFields();";
    html += "</script>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSetTime() {
  if (server.hasArg("ts")) {
    long unixTime = server.arg("ts").toInt();

    struct timeval tv;
    tv.tv_sec = unixTime;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    timeSynced = true;
    distressMode = false;
    Serial.printf("Time successfully synced from browser fallback: %ld\n", unixTime);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing timestamp");
  }
}

void handleStart() {
  preferences.putBool("running", true);
  isRunning = true;
  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><style>body{font-family: Arial; text-align: center; margin-top: 50px;}</style></head><body><h2>Collection Started!</h2><p>Portal closing. The sensor will begin sampling.</p></body></html>";
  server.send(200, "text/html", html);
  delay(1000);
  exitPortal = true;
}

void handleStop() {
  preferences.putBool("running", false);
  isRunning = false;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleClear() {
  if (LittleFS.exists(csvFilename.c_str())) {
    LittleFS.remove(csvFilename.c_str());
  }
  bootCount = 0;
  prevStatus = -1;
  lastLoggedEpoch = 0;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSave() {
  if (server.hasArg("id")) preferences.putString("id", server.arg("id"));
  if (server.hasArg("sleep")) preferences.putInt("sleep", max(server.arg("sleep").toInt(), 1L));
  if (server.hasArg("max_log_int")) preferences.putInt("max_log_int", max(server.arg("max_log_int").toInt(), 1L));
  if (server.hasArg("rss_thresh")) preferences.putFloat("rss_thresh", server.arg("rss_thresh").toFloat());
  if (server.hasArg("deployment_mode")) preferences.putInt("deploy_mode", server.arg("deployment_mode").toInt());
  if (server.hasArg("wifi_ssid")) preferences.putString("wifi_ssid", server.arg("wifi_ssid"));
  if (server.hasArg("wifi_pass")) preferences.putString("wifi_pass", server.arg("wifi_pass"));

  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><style>body{font-family: Arial; text-align: center; margin-top: 50px;}</style></head><body><h2>Settings Saved!</h2><p>Rebooting sensor to apply...</p></body></html>";
  server.send(200, "text/html", html);
  delay(1000);
  ESP.restart();
}

void handleDownload() {
  File file = LittleFS.open(csvFilename.c_str(), FILE_READ);
  if (!file) {
    server.send(404, "text/plain", "File not found in internal flash memory.");
    return;
  }

  String downloadName = sensorID + ".csv";
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  server.streamFile(file, "text/csv");
  file.close();
}

bool recoverTimeFromWifi(struct tm* timeCheck) {
  if (!wifiRecoveryEnabled() || wifiSSID.length() == 0) return false;

  Serial.println("Mode: WiFi Auto-Recovery. Connecting just to fetch time...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }

  bool recovered = false;
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected! Requesting NTP...");
    configTzTime(tzInfo, ntpServer);
    if (getLocalTime(timeCheck, 10000) && timeCheck->tm_year > 100) {
      recovered = true;
      Serial.println("NTP Sync Successful! Resuming data collection.");
    } else {
      Serial.println("NTP Server timeout.");
    }
  } else {
    Serial.println("\nWiFi Connection Failed.");
  }

  turnWifiOff();
  return recovered;
}

void runPortal(bool emergencyAlarm) {
  Serial.println("\n--- Entering AP Portal Mode ---");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/settime", handleSetTime);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/clear", handleClear);
  server.on("/save", handleSave);
  server.on("/download", handleDownload);
  server.begin();

  Serial.println("Web Server Started. Portal will remain open for 3 minutes.");

  unsigned long portalStartTime = millis();
  while ((millis() - portalStartTime < 180000) && !exitPortal) {
    server.handleClient();

    if (emergencyAlarm || distressMode) {
      int cycle = millis() % 1000;
      if (cycle < 100 || (cycle > 200 && cycle < 300)) {
        setStatusLed(true);
      } else {
        setStatusLed(false);
      }
    } else {
      setStatusLed((millis() / 500) % 2 == 0);
    }
    delay(10);
  }

  server.stop();
  WiFi.softAPdisconnect(true);
  turnWifiOff();
  setStatusLed(false);
  isRunning = preferences.getBool("running", false);
}

bool collectMeasurement(float* rmsOut) {
  if (!gyro_init()) return false;

  // Preserve the proven cold-start/wake transient fix from V2.
  delay(100);
  for (int i = 0; i < 20; i++) {
    if (!gyro_signals()) return false;
    delay(5);
  }

  float alpha = RC / (RC + (dT / 1000.0));
  float sumSq = 0.0;

  for (int i = 0; i < sampleCount; i++) {
    if (!gyro_signals()) return false;
    for (int axis = 0; axis < 3; axis++) {
      Accel[axis][i] = instAccelXYZ[axis];
      if (i == 0) {
        xyzHPF[axis][i] = 0.0;
      } else {
        xyzHPF[axis][i] = alpha * (xyzHPF[axis][i - 1] + Accel[axis][i] - Accel[axis][i - 1]);
      }
    }
    float mag = sqrt(pow(xyzHPF[0][i], 2) + pow(xyzHPF[1][i], 2) + pow(xyzHPF[2][i], 2));
    sumSq += mag * mag;
    delay(dT);
  }

  *rmsOut = sqrt(sumSq / sampleCount);
  return true;
}

bool writeMeasurementIfNeeded(float rmsValue, time_t nowEpoch) {
  machineStatus = (rmsValue > RSSsafety) ? 1 : 0;
  Serial.printf("Calculated RMS: %.4f | Status: %d\n", rmsValue, machineStatus);

  bool stateChanged = machineStatus != prevStatus;
  bool heartbeatDue = lastLoggedEpoch == 0 ||
                      nowEpoch < lastLoggedEpoch ||
                      nowEpoch - lastLoggedEpoch >= (time_t)maxLoggedIntervalMins * 60;
  bool shouldWrite = stateChanged || heartbeatDue;

  if (!shouldWrite) {
    Serial.println("Skipping flash write - No state change.");
    prevStatus = machineStatus;
    return true;
  }

  if (!flashMounted) {
    Serial.println("[STORAGE ERROR] Measurement was valid but flash is unavailable.");
    prevStatus = machineStatus;
    return false;
  }

  struct tm timeinfo;
  char timeString[64];
  if (getLocalTime(&timeinfo)) {
    strftime(timeString, sizeof(timeString), "%m/%d/%Y %H:%M:%S", &timeinfo);
  } else {
    Serial.println("[TIME ERROR] Valid measurement not logged because calendar time is unavailable.");
    return false;
  }

  bool fileExists = LittleFS.exists(csvFilename.c_str());
  File file = LittleFS.open(csvFilename.c_str(), FILE_APPEND);
  if (!file) {
    Serial.println("[STORAGE ERROR] Failed to open CSV for appending.");
    return false;
  }

  if (!fileExists) file.println("Month/DD/YYYY HH:MM, Status, RSS value");
  file.printf("%s,%d,%.4f\n", timeString, machineStatus, rmsValue);
  file.close();

  lastLoggedEpoch = nowEpoch;
  prevStatus = machineStatus;
  Serial.println("Data saved to Flash.");
  return true;
}

unsigned long remainingIntervalMs(unsigned long activeMs, unsigned long nextBootEstimateMs) {
  uint64_t targetMs = (uint64_t)max(sleepSeconds, 1) * 1000ULL;
  uint64_t usedMs = (uint64_t)activeMs + nextBootEstimateMs;
  if (usedMs + 100ULL < targetMs) return (unsigned long)(targetMs - usedMs);
  return 100UL;
}

void ensureUsbKeepAliveSchedule() {
  if (nextUsbKeepAliveUs == 0) {
    nextUsbKeepAliveUs = esp_timer_get_time() + (uint64_t)USB_KEEPALIVE_INTERVAL_MS * 1000ULL;
  }
}

void runUsbKeepAlivePulse() {
  Serial.println("[POWER] Starting validated 1-second USB-bank keep-alive pulse.");
  setCpuFrequencyMhz(normalCpuFrequencyMhz);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("OnOff-KeepAlive", AP_PASS);
  smartDelay(USB_KEEPALIVE_DURATION_MS);
  WiFi.softAPdisconnect(true);
  turnWifiOff();
  setCpuFrequencyMhz(USB_BASELINE_CPU_MHZ);
  Serial.println("[POWER] USB-bank keep-alive pulse complete.");
}

void serviceDueUsbKeepAlive() {
  ensureUsbKeepAliveSchedule();
  uint64_t nowUs = esp_timer_get_time();
  if (nowUs < nextUsbKeepAliveUs) return;

  runUsbKeepAlivePulse();
  nowUs = esp_timer_get_time();
  do {
    nextUsbKeepAliveUs += (uint64_t)USB_KEEPALIVE_INTERVAL_MS * 1000ULL;
  } while (nextUsbKeepAliveUs <= nowUs);
}

void waitInUsbBankMode(unsigned long waitMs) {
  turnWifiOff();
  Serial.printf("[POWER] USB-bank wait at %u MHz for %lu ms; pulse=%lu/%lu ms.\n",
                USB_BASELINE_CPU_MHZ, waitMs,
                (unsigned long)USB_KEEPALIVE_DURATION_MS,
                (unsigned long)USB_KEEPALIVE_INTERVAL_MS);
  Serial.flush();
  setCpuFrequencyMhz(USB_BASELINE_CPU_MHZ);
  ensureUsbKeepAliveSchedule();

  uint64_t waitEndUs = esp_timer_get_time() + (uint64_t)waitMs * 1000ULL;
  while (esp_timer_get_time() < waitEndUs) {
    serviceDueUsbKeepAlive();
    uint64_t nowUs = esp_timer_get_time();
    if (nowUs >= waitEndUs) break;

    uint64_t untilEndUs = waitEndUs - nowUs;
    uint64_t untilPulseUs = nextUsbKeepAliveUs > nowUs ? nextUsbKeepAliveUs - nowUs : 0;
    uint64_t chunkUs = untilEndUs < untilPulseUs ? untilEndUs : untilPulseUs;
    uint64_t availableMs = chunkUs / 1000ULL;
    unsigned long chunkMs = (unsigned long)(availableMs < 10ULL ? availableMs : 10ULL);
    smartDelay(max(chunkMs, 1UL));
  }
  setCpuFrequencyMhz(normalCpuFrequencyMhz);
}

void prepareUsbBankMeasurement() {
  if (!isUsbBankMode()) return;
  ensureUsbKeepAliveSchedule();
  uint64_t nowUs = esp_timer_get_time();
  if (nextUsbKeepAliveUs <= nowUs) {
    serviceDueUsbKeepAlive();
    return;
  }

  uint64_t guardUs = (uint64_t)MEASUREMENT_GUARD_MS * 1000ULL;
  if (nextUsbKeepAliveUs - nowUs <= guardUs) {
    unsigned long waitMs = (unsigned long)((nextUsbKeepAliveUs - nowUs + 999ULL) / 1000ULL);
    waitInUsbBankMode(waitMs);
    serviceDueUsbKeepAlive();
  }
}

void enterCubeTimedSleep(unsigned long waitMs) {
#if CONFIG_IDF_TARGET_ESP32C3
  Serial.printf("[POWER] C3 power-cube light sleep for %lu ms (BOOT wake enabled).\n", waitMs);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)waitMs * 1000ULL);
  gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();
  gpio_wakeup_disable((gpio_num_t)BUTTON_PIN);

  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("[INTERRUPT] C3 BOOT wake detected; opening portal.");
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
    runPortal(false);
  }
#else
  Serial.printf("[POWER] S3 power-cube deep sleep for %lu ms.\n", waitMs);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)waitMs * 1000ULL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  esp_deep_sleep_start();
#endif
}

void waitForCubeButton() {
#if CONFIG_IDF_TARGET_ESP32C3
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  while (!isRunning) {
    gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    gpio_wakeup_disable((gpio_num_t)BUTTON_PIN);
    if (digitalRead(BUTTON_PIN) == LOW) {
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      runPortal(distressMode);
    }
  }
#else
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
  esp_deep_sleep_start();
#endif
}

void idleOrDistress() {
  if (distressMode) {
    if (isUsbBankMode()) {
      turnWifiOff();
      setCpuFrequencyMhz(USB_BASELINE_CPU_MHZ);
      while (true) {
        setStatusLed(true);
        delay(30);
        setStatusLed(false);
        delay(100);
        setStatusLed(true);
        delay(30);
        setStatusLed(false);
        waitInUsbBankMode(4000);
      }
    }

    while (distressMode && !isRunning) {
      setStatusLed(true);
      delay(30);
      setStatusLed(false);
      delay(100);
      setStatusLed(true);
      delay(30);
      setStatusLed(false);
      Serial.println("Distress Mode Active. Sleeping for 4 seconds...");
      enterCubeTimedSleep(4000);
    }
    if (isRunning) return;
  }

  if (isUsbBankMode()) {
    Serial.println("System IDLE. Maintaining USB-bank load until button press.");
    turnWifiOff();
    setCpuFrequencyMhz(USB_BASELINE_CPU_MHZ);
    while (true) waitInUsbBankMode(60000);
  }

  Serial.println(CUBE_USES_LIGHT_SLEEP
                   ? "System IDLE. Entering C3 light sleep until BOOT press."
                   : "System IDLE. Entering S3 deep sleep until BOOT press.");
  Serial.flush();
  waitForCubeButton();
}

// --- Main Logic ---
void setup() {
  unsigned long setupStartMs = millis();
  Serial.begin(115200);

  setenv("TZ", tzInfo, 1);
  tzset();

  if (HAS_STATUS_LED) {
    pinMode(LED_PIN, OUTPUT);
    setStatusLed(false);
  }
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(100);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed! Check Partition Scheme.");
    flashMounted = false;
    blinkLED(20, 50);
  } else {
    Serial.println("LittleFS Mount Successful.");
    flashMounted = true;
  }

  preferences.begin("config", false);
  sensorID = preferences.getString("id", "OnOffSensor");
  sleepSeconds = max(preferences.getInt("sleep", 10), 1L);
  maxLoggedIntervalMins = max(preferences.getInt("max_log_int", 5), 1L);
  RSSsafety = preferences.getFloat("rss_thresh", 0.028);
  isRunning = preferences.getBool("running", false);

  if (preferences.isKey("deploy_mode")) {
    deploymentMode = preferences.getInt("deploy_mode", USB_BANK_NO_WIFI);
  } else {
    int oldSyncMode = preferences.getInt("sync_mode", 0);
    deploymentMode = oldSyncMode == 1 ? USB_BANK_WIFI : USB_BANK_NO_WIFI;
    preferences.putInt("deploy_mode", deploymentMode);
  }
  if (deploymentMode < USB_BANK_NO_WIFI || deploymentMode > POWER_CUBE_WIFI) {
    deploymentMode = USB_BANK_NO_WIFI;
  }

  wifiSSID = preferences.getString("wifi_ssid", "");
  wifiPass = preferences.getString("wifi_pass", "");
  csvFilename = "/" + sensorID + ".csv";
  normalCpuFrequencyMhz = getCpuFrequencyMhz();
  Serial.printf("[V5] version=%s board=%s normal_cpu_mhz=%lu status_led=%s\n",
                FIRMWARE_VERSION, BOARD_NAME, (unsigned long)normalCpuFrequencyMhz,
                HAS_STATUS_LED ? "yes" : "no");

  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  bool openPortal = false;
  bool emergencyAlarm = false;

  struct tm timeCheck;
  if (!getLocalTime(&timeCheck) || timeCheck.tm_year <= 100) {
    timeSynced = false;
  } else {
    timeSynced = true;
    distressMode = false;
  }

  if (digitalRead(BUTTON_PIN) == LOW || wakeReason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Button Wake/Hold Detected! Opening Portal.");
    openPortal = true;
    distressMode = false;
    while (digitalRead(BUTTON_PIN) == LOW) delay(10);
  } else if (wakeReason != ESP_SLEEP_WAKEUP_TIMER) {
    if (isRunning) {
      Serial.println("Power recovered. Checking time...");
    } else {
      Serial.println("Clean boot (Idle). Opening portal...");
      openPortal = true;
    }
  }

  if (isRunning && !timeSynced && !openPortal) {
    Serial.println("CRITICAL: Time lost due to power failure.");
    if (recoverTimeFromWifi(&timeCheck)) {
      timeSynced = true;
      distressMode = false;
    }

    if (!timeSynced) {
      Serial.println("Time Recovery Failed. Triggering visual alarm and forcing portal.");
      isRunning = false;
      preferences.putBool("running", false);
      openPortal = true;
      emergencyAlarm = true;
      distressMode = true;
    }
  }

  if (openPortal) runPortal(emergencyAlarm);

  if (!isRunning || !timeSynced) idleOrDistress();

  turnWifiOff();
  if (isUsbBankMode()) {
    nextUsbKeepAliveUs = esp_timer_get_time() + (uint64_t)USB_KEEPALIVE_INTERVAL_MS * 1000ULL;
  }
  Serial.printf("[MODE] deployment=%d usb_bank=%s wifi_recovery=%s\n",
                deploymentMode,
                isUsbBankMode() ? "yes" : "no",
                wifiRecoveryEnabled() ? "yes" : "no");
  Serial.printf("[TIMING] setup_ready_ms=%lu\n", millis() - setupStartMs);
}

void loop() {
  prepareUsbBankMeasurement();
  unsigned long cycleStartMs = millis();
  bootCount++;
  Serial.printf("Boot/Cycle Count: %d\n", bootCount);
  blinkLED(2, 200);

  float rmsValue = 0.0;
  if (!collectMeasurement(&rmsValue)) {
    Serial.println("[SENSOR ERROR] Measurement discarded; no OFF row will be written.");
  } else {
    time_t nowEpoch;
    time(&nowEpoch);
    writeMeasurementIfNeeded(rmsValue, nowEpoch);
  }

  unsigned long activeMs = millis() - cycleStartMs;
  if (isUsbBankMode()) {
    unsigned long waitMs = remainingIntervalMs(activeMs, 0);
    Serial.printf("[TIMING] active_ms=%lu wait_ms=%lu target_ms=%lu\n",
                  activeMs, waitMs, (unsigned long)sleepSeconds * 1000UL);
    waitInUsbBankMode(waitMs);
    return;
  }

  // The next deep-sleep boot has approximately the same setup overhead as this
  // boot. Ignore portal-duration outliers when estimating the next cycle.
  unsigned long bootEstimateMs = cycleStartMs <= 5000UL ? cycleStartMs : 0UL;
  unsigned long waitMs = remainingIntervalMs(activeMs, bootEstimateMs);
  Serial.printf("[TIMING] active_ms=%lu wait_ms=%lu next_boot_estimate_ms=%lu target_ms=%lu\n",
                activeMs, waitMs, bootEstimateMs, (unsigned long)sleepSeconds * 1000UL);
  enterCubeTimedSleep(waitMs);
}
