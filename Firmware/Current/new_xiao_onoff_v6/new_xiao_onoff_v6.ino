#include <WiFi.h>
#include <Wire.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>
#include <mqtt_client.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Forward declaration keeps Arduino's generated function prototypes valid.
struct PortalDataState;

// AP Mode Configuration
#define AP_SSID "OnOff"
#define AP_PASS "onoff123"
constexpr const char* FIRMWARE_VERSION = "6.1.2";

// EzAmp-compatible MQTT defaults. V6 changes the device path from ezamp to
// onoff while allowing a custom server and topic prefix in the field portal.
constexpr const char* DEFAULT_MQTT_BROKER = "broker1.healthybuildingsolution.com";
constexpr uint16_t DEFAULT_MQTT_PORT = 1883;
constexpr const char* DEFAULT_MQTT_USERNAME = "ezamp";
constexpr const char* DEFAULT_MQTT_PASSWORD = "ezamp123";
constexpr const char* DEFAULT_MQTT_TOPIC_PREFIX = "emqx";
constexpr const char* MQTT_FIXED_TOPIC_PATH = "onoff/v1/devices";
constexpr uint32_t MQTT_WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t MQTT_ACK_TIMEOUT_MS = 5000;
constexpr uint32_t MQTT_RETRY_BACKOFF_MS = 60000;
constexpr uint8_t MQTT_CATCHUP_ROWS_PER_CYCLE = 8;
constexpr uint8_t MQTT_CURSOR_CHECKPOINT_ROWS = 10;

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

enum MqttMode {
  MQTT_DISABLED = 0,
  MQTT_DEFAULT_SERVER = 1,
  MQTT_CUSTOM_SERVER = 2
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
int mqttMode = MQTT_DISABLED;
String mqttBroker;
uint16_t mqttPort = DEFAULT_MQTT_PORT;
String mqttUsername;
String mqttPassword;
String mqttTopicPrefix;
String deviceUID;
uint32_t deploymentID = 1;
bool mqttStayAwake = false;
uint32_t mqttCursorOffset = 0;
uint32_t mqttCheckpointOffset = 0;
uint8_t mqttRowsSinceCheckpoint = 0;
volatile bool mqttBacklogPending = false;
volatile uint32_t mqttRetryNotBeforeMs = 0;

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

struct MqttCsvRow {
  time_t epoch;
  int status;
  float rms;
  uint32_t nextOffset;
};

QueueHandle_t mqttQueue = nullptr;
TaskHandle_t mqttTaskHandle = nullptr;
volatile bool mqttWorkerPending = false;
volatile bool mqttWorkerActive = false;
volatile bool measurementInProgress = false;
esp_mqtt_client_handle_t activeMqttClient = nullptr;
volatile bool mqttBrokerConnected = false;
volatile int mqttLastAcknowledgedMessageId = -1;
String activeMqttBroker;
String activeMqttUsername;
String activeMqttPassword;
String activeMqttClientID;

void clearMqttCursorState();
void persistMqttCursor(bool force);
String effectiveMqttTopicPrefix();
String mqttTopic();

bool isUsbBankMode() {
  return deploymentMode == USB_BANK_NO_WIFI || deploymentMode == USB_BANK_WIFI;
}

bool wifiRecoveryEnabled() {
  return deploymentMode == USB_BANK_WIFI || deploymentMode == POWER_CUBE_WIFI;
}

bool mqttEnabled() {
  return mqttMode == MQTT_DEFAULT_SERVER || mqttMode == MQTT_CUSTOM_SERVER;
}

bool mqttAlwaysConnected() {
  return mqttEnabled() && mqttStayAwake;
}

bool mqttRetryDue() {
  return mqttRetryNotBeforeMs == 0 ||
         (int32_t)(millis() - mqttRetryNotBeforeMs) >= 0;
}

int networkUseMode() {
  if (mqttEnabled()) return 2;
  return wifiRecoveryEnabled() ? 1 : 0;
}

String readDeviceUID() {
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return "unknown";
  char value[13];
  snprintf(value, sizeof(value), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(value);
}

void turnWifiOff() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  // Wi-Fi shutdown completes asynchronously. Give the stack time to reach the
  // stopped state before a portal or MQTT worker tries to initialize it again.
  delay(100);
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
      if (mqttEnabled()) persistMqttCursor(true);
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

struct PortalDataState {
  uint32_t fileSize;
  uint32_t pendingBytes;
  uint32_t pendingRows;
  bool cursorMatchesFile;
};

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

String urlEncode(const String& value) {
  const char* hex = "0123456789ABCDEF";
  String encoded;
  for (size_t i = 0; i < value.length(); i++) {
    uint8_t c = (uint8_t)value[i];
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (safe) {
      encoded += (char)c;
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

String normalizeCsvPath(String value) {
  value.trim();
  if (!value.startsWith("/")) value = "/" + value;
  return value;
}

bool isSafeCsvPath(const String& value) {
  return value.startsWith("/") && value.endsWith(".csv") &&
         value.indexOf("..") < 0 && value.indexOf('\\') < 0 &&
         value.indexOf('/', 1) < 0;
}

bool isValidSensorID(String value) {
  value.trim();
  if (value.length() == 0 || value.length() > 48) return false;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '/' || c == '\\' || c == ',' || c == '<' || c == '>' ||
        c == '"' || c == '\r' || c == '\n') return false;
  }
  return value != "." && value != "..";
}

bool isValidMqttPrefix(String value) {
  value.trim();
  if (value.length() == 0 || value.length() > 96 || value.startsWith("/") ||
      value.endsWith("/") || value.indexOf("//") >= 0) return false;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '+' || c == '#' || c == ',' || c == '<' || c == '>' ||
        c == '"' || c == '\r' || c == '\n' || c == ' ') return false;
  }
  return true;
}

uint32_t countRowsFromOffset(const String& path, uint32_t offset) {
  File file = LittleFS.open(path.c_str(), FILE_READ);
  if (!file || offset > file.size() || !file.seek(offset)) {
    if (file) file.close();
    return 0;
  }
  uint32_t rows = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0 && !line.startsWith("Month/")) rows++;
  }
  file.close();
  return rows;
}

PortalDataState portalDataState() {
  PortalDataState state = {0, 0, 0, false};
  File file = LittleFS.open(csvFilename.c_str(), FILE_READ);
  if (!file) return state;
  state.fileSize = file.size();
  file.close();

  String savedFile = preferences.getString("mqfile", "");
  uint32_t savedOffset = preferences.getUInt("mqoff", state.fileSize);
  state.cursorMatchesFile = preferences.isKey("mqoff") && savedFile == csvFilename &&
                            savedOffset <= state.fileSize;
  if (state.cursorMatchesFile && savedOffset < state.fileSize) {
    state.pendingBytes = state.fileSize - savedOffset;
    state.pendingRows = countRowsFromOffset(csvFilename, savedOffset);
  }
  return state;
}

String orphanCsvWarningHtml() {
  if (!flashMounted) return "";
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) return "";

  String rows;
  File file = root.openNextFile();
  while (file) {
    String path = normalizeCsvPath(file.name());
    if (!file.isDirectory() && path.endsWith(".csv") && path != csvFilename) {
      String display = path.substring(1);
      rows += "<div class='orphan'><strong>" + htmlEscape(display) + "</strong> (" +
              String(file.size() / 1024.0, 2) + " KB)<br>";
      rows += "<a href='/file-download?file=" + urlEncode(path) + "'>Download</a> | ";
      rows += "<a href='/file-delete?file=" + urlEncode(path) +
              "' onclick=\"return confirm('Delete recovered file " + htmlEscape(display) +
              "? Download it first if needed.');\">Delete</a></div>";
    }
    file = root.openNextFile();
  }
  root.close();
  if (rows.length() == 0) return "";
  return "<div class='warning'><strong>Older CSV data was found.</strong><br>"
         "Download or delete each recovered file before deployment." + rows + "</div>";
}

String messagePage(const String& title, const String& message, int statusCode = 409) {
  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:Arial;text-align:center;background:#f4f6f9;padding:25px;}"
          ".card{max-width:430px;margin:auto;background:white;padding:25px;border-radius:12px;}"
          "a{display:block;margin-top:16px;color:#0069d9;}</style></head><body><div class='card'>";
  html += "<h2>" + htmlEscape(title) + "</h2><p>" + message + "</p>";
  html += "<a href='/'>Return to Sensor Portal</a></div></body></html>";
  server.send(statusCode, "text/html", html);
  return html;
}

// --- Web Server Functions (AP Mode) ---
void handleRoot() {
  PortalDataState dataState = portalDataState();
  String orphanWarning = orphanCsvWarningHtml();
  size_t freeFilesystemBytes = 0;
  if (flashMounted) {
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    freeFilesystemBytes = usedBytes < totalBytes ? totalBytes - usedBytes : 0;
  }
  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<style>body{font-family: Arial; text-align: center; margin-top: 30px; background-color: #f4f6f9; color: #333;}";
  html += ".card{max-width: 400px; margin: 0 auto; background: white; padding: 30px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.1);}";
  html += "button{color: white; padding: 15px 32px; font-size: 18px; border: none; border-radius: 8px; cursor: pointer; width: 100%; margin-top: 10px; transition: 0.3s;}";
  html += ".btn-start{background-color: #28a745;} .btn-start:hover:enabled{background-color: #218838;}";
  html += ".btn-stop{background-color: #dc3545;} .btn-stop:hover{background-color: #c82333;}";
  html += ".btn-dl{background-color: #007bff;} .btn-dl:hover{background-color: #0056b3;}";
  html += ".btn-clear{background-color: #ff9800;} .btn-clear:hover{background-color: #e68a00;}";
  html += ".info{float:right;width:34px;height:34px;padding:0;margin:0;background:#6c757d;border-radius:50%;font-size:20px;}";
  html += ".warning{background:#fff3cd;color:#664d03;padding:12px;border-radius:8px;margin:12px 0;text-align:left;font-size:13px;}";
  html += ".safe{background:#d1e7dd;color:#0f5132;padding:10px;border-radius:8px;margin:12px 0;font-size:13px;}";
  html += ".orphan{margin-top:8px;padding-top:8px;border-top:1px solid #d6b656;}";
  html += ".modal{display:none;position:fixed;z-index:5;left:0;top:0;width:100%;height:100%;overflow:auto;background:rgba(0,0,0,.5);}";
  html += ".modalbox{background:white;margin:8% auto;padding:22px;border-radius:12px;max-width:430px;text-align:left;}";
  html += ".close{float:right;font-size:28px;cursor:pointer;}code{font-size:11px;word-break:break-all;}";
  html += "input, select{width: 100%; padding: 10px; margin: 8px 0; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box;}</style>";

  html += "<script>";
  html += "function toggleNetworkFields() {";
  html += "  var networkMode = document.getElementById('network_mode').value;";
  html += "  var mqttMode = document.getElementById('mqtt_server').value;";
  html += "  var wifiDiv = document.getElementById('wifi_settings');";
  html += "  var mqttDiv = document.getElementById('mqtt_settings');";
  html += "  var customDiv = document.getElementById('mqtt_custom_settings');";
  html += "  wifiDiv.style.display = (networkMode != '0') ? 'block' : 'none';";
  html += "  mqttDiv.style.display = (networkMode == '2') ? 'block' : 'none';";
  html += "  customDiv.style.display = (networkMode == '2' && mqttMode == '2') ? 'block' : 'none';";
  html += "}";
  html += "function openInfo(){document.getElementById('infoModal').style.display='block';}";
  html += "function closeInfo(){document.getElementById('infoModal').style.display='none';}";
  html += "function updateGeneratedTopic(){var p=document.getElementById('mqtt_prefix');var o=document.getElementById('generated_topic');if(p&&o){o.textContent=p.value.replace(/^\\/+|\\/+$/g,'')+'/onoff/v1/devices/" + deviceUID + "/readings';}}";
  html += "function durationText(s){var d=s/86400;if(d>=365)return(d/365).toFixed(1)+' years';if(d>=1)return d.toFixed(1)+' days';return(s/3600).toFixed(1)+' hours';}";
  html += "function updateCsvEstimate(){var s=Math.max(parseInt(document.getElementById('sample_interval').value)||1,1);var m=Math.max(parseInt(document.getElementById('maximum_csv_interval').value)||1,1);var rows=Math.floor(" + String((unsigned long)freeFilesystemBytes) + "*0.9/32);var unchanged=Math.max(s,m*60);var e=document.getElementById('csv_capacity');if(!e)return;if(rows<1){e.innerHTML='<strong>Estimated CSV capacity:</strong> unavailable because no free filesystem space was reported.';return;}e.innerHTML='<strong>Estimated remaining CSV capacity:</strong> about '+rows.toLocaleString()+' rows.<br>No status changes: '+durationText(rows*unchanged)+'.<br>Status changes every sample: '+durationText(rows*s)+'.<br><small>Uses current free space, 32 bytes per row, and a 10% filesystem reserve. Actual duration depends on status changes.</small>'; }";
  html += "</script></head>";

  html += "<body><div class=\"card\"><button class='info' type='button' onclick='openInfo()' aria-label='Operation help'>i</button><h2>OnOff Sensor Portal</h2>";
  html += "<p><strong>Sensor ID:</strong> " + htmlEscape(sensorID) + "</p>";
  html += "<p style='font-size:12px;color:#666;'><strong>Device UID:</strong> " + deviceUID +
          "<br><strong>Deployment:</strong> " + String(deploymentID) + "</p>";
  html += "<p style=\"font-size:12px; color:#666;\"><strong>Firmware:</strong> V" + String(FIRMWARE_VERSION) + " | " + String(BOARD_NAME) + "</p>";

  html += "<div id='infoModal' class='modal'><div class='modalbox'><span class='close' onclick='closeInfo()'>&times;</span>";
  html += "<h3>Recommended Operation Flow</h3><p><strong>Initial deployment</strong></p><ol>"
          "<li>Enter the Sensor ID and collection settings.</li><li>Select power and network use.</li>"
          "<li>If using MQTT, enter the server details and MQTT Topic Prefix.</li>"
          "<li>Confirm the generated topic, then Save and Start.</li></ol>";
  html += "<p><strong>Safe reuse</strong></p><ol><li>Stop collection.</li>"
          "<li>If MQTT records are pending, resume the old deployment or download and explicitly discard them.</li>"
          "<li>Download the CSV.</li><li>Delete the current CSV.</li>"
          "<li>Select Prepare for New Deployment, enter the next Sensor ID, then Start.</li></ol>";
  html += "<p>The Device UID is permanent. Deployment numbers advance automatically.</p></div></div>";

  if (dataState.pendingRows > 0) {
    html += "<div class='warning'><strong>MQTT upload pending:</strong> " + String(dataState.pendingRows) +
            " CSV record(s) have not been acknowledged. Do not change the MQTT destination or begin a new deployment yet.</div>";
  } else if (mqttEnabled() && dataState.fileSize > 0) {
    html += "<div class='safe'>MQTT status: all saved records acknowledged.</div>";
  }
  html += orphanWarning;

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

  if (dataState.fileSize > 0) {
    html += "<a href=\"/download\"><button class=\"btn-dl\">Download CSV</button></a>";
  }

  if (!isRunning && dataState.fileSize > 0) {
    String clearHref = dataState.pendingRows > 0 ? "/clear?discard_pending=1" : "/clear";
    String clearWarning = dataState.pendingRows > 0
                              ? "This CSV has " + String(dataState.pendingRows) + " unacknowledged MQTT record(s). Delete it and cancel those uploads? Download it first."
                              : "Delete " + sensorID + ".csv? Download it first if it is needed.";
    html += "<a href='" + clearHref + "' onclick=\"return confirm('" + htmlEscape(clearWarning) +
            "');\"><button class='btn-clear'>Delete Current CSV</button></a>";
  }

  if (!isRunning && dataState.fileSize == 0 && dataState.pendingRows == 0) {
    html += "<a href='/new-deployment' onclick=\"return confirm('Advance to a new deployment? The Device UID will stay the same.');\">"
            "<button class='btn-clear' style='background:#6f42c1;'>Prepare for New Deployment</button></a>";
  }

  html += "<hr style=\"margin: 25px 0;\"><h3 style=\"text-align:left;\">Sensor Configuration</h3>";
  html += "<form action=\"/save\" method=\"POST\">";
  html += "<div style=\"text-align: left;\">";
  html += "<label>Sensor ID (CSV Filename):</label><br><input type=\"text\" name=\"id\" value=\"" + htmlEscape(sensorID) + "\"><br>";
  html += "<label>Sampling Interval (Sec):</label><br><input id=\"sample_interval\" type=\"number\" min=\"1\" name=\"sleep\" value=\"" + String(sleepSeconds) + "\" oninput=\"updateCsvEstimate()\"><br>";
  html += "<label>Maximum Time Between CSV Rows (Minutes):</label><br><input id=\"maximum_csv_interval\" type=\"number\" min=\"1\" name=\"max_log_int\" value=\"" + String(maxLoggedIntervalMins) + "\" oninput=\"updateCsvEstimate()\"><br>";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px;\"><em>A row is saved sooner whenever the ON/OFF status changes.</em></p>";
  html += "<div id=\"csv_capacity\" style=\"background:#eef5ff;padding:10px;border-radius:6px;margin:8px 0 14px;font-size:12px;line-height:1.5;\"></div>";

  html += "<label>Vibration Threshold (RMS):</label><br>";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px; margin-bottom:4px;\"><em>(Default: 0.028 | Lowest recorded: 0.0068. Check CSV baseline to avoid false ONs from background noise.)</em></p>";
  html += "<input type=\"number\" step=\"0.0001\" name=\"rss_thresh\" value=\"" + String(RSSsafety, 4) + "\"><br>";

  html += "<label>Power Source:</label><br>";
  html += "<select id=\"power_source\" name=\"power_source\">";
  html += "<option value=\"0\" " + String(isUsbBankMode() ? "selected" : "") + ">External USB Battery</option>";
  html += "<option value=\"1\" " + String(!isUsbBankMode() ? "selected" : "") + ">Power Cube / Wall Adapter</option>";
  html += "</select><br>";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px;\"><em>External USB Battery modes automatically keep compatible power banks awake.</em></p>";

  int selectedNetworkUse = networkUseMode();
  html += "<label>Network Use:</label><br>";
  html += "<select id=\"network_mode\" name=\"network_mode\" onchange=\"toggleNetworkFields()\">";
  html += "<option value=\"0\" " + String(selectedNetworkUse == 0 ? "selected" : "") + ">No Network Wi-Fi</option>";
  html += "<option value=\"1\" " + String(selectedNetworkUse == 1 ? "selected" : "") + ">Wi-Fi for Time Recovery</option>";
  html += "<option value=\"2\" " + String(selectedNetworkUse == 2 ? "selected" : "") + ">Wi-Fi for Time Recovery + MQTT</option>";
  html += "</select><br>";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px;\"><em>MQTT uploads every CSV row: status-change rows and rows saved when the maximum time between CSV rows is reached.</em></p>";

  String displayStyle = selectedNetworkUse != 0 ? "block" : "none";
  html += "<div id=\"wifi_settings\" style=\"display:" + displayStyle + "; background-color:#e9ecef; padding:10px; border-radius:5px; margin-top:10px;\">";
  html += "<p style=\"font-size:12px; margin-top:0;\"><strong>Wi-Fi:</strong> Used for MQTT uploads and automatic time recovery after power loss.</p>";
  html += "<label>WiFi SSID:</label><input type=\"text\" name=\"wifi_ssid\" value=\"" + htmlEscape(wifiSSID) + "\">";
  html += "<label>WiFi Password:</label><input type=\"password\" name=\"wifi_pass\" value=\"" + htmlEscape(wifiPass) + "\">";
  html += "</div>";

  String mqttDisplayStyle = selectedNetworkUse == 2 ? "block" : "none";
  html += "<div id=\"mqtt_settings\" style=\"display:" + mqttDisplayStyle + "; background-color:#eef5ff; padding:10px; border-radius:5px; margin-top:10px;\">";
  html += "<label>MQTT Server:</label><select id=\"mqtt_server\" name=\"mqtt_server\" onchange=\"toggleNetworkFields()\">";
  html += "<option value=\"1\" " + String(mqttMode != MQTT_CUSTOM_SERVER ? "selected" : "") + ">Default</option>";
  html += "<option value=\"2\" " + String(mqttMode == MQTT_CUSTOM_SERVER ? "selected" : "") + ">Custom</option>";
  html += "</select>";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px;\"><em>Battery mode disconnects after upload. Wall mode keeps the MQTT connection available.</em></p>";
  html += "<label>MQTT Topic Prefix:</label><input id=\"mqtt_prefix\" type=\"text\" name=\"mqtt_prefix\" value=\"" + htmlEscape(mqttTopicPrefix) + "\" placeholder=\"emqx\" oninput=\"updateGeneratedTopic()\">";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px;\"><em>Choose the root topic used by your MQTT system. The remaining path is fixed by firmware.</em></p>";
  html += "<p style='font-size:12px;'><strong>Generated Topic:</strong><br><code id='generated_topic'>" + htmlEscape(mqttTopic()) + "</code></p>";
  String customDisplayStyle = mqttMode == MQTT_CUSTOM_SERVER ? "block" : "none";
  html += "<div id=\"mqtt_custom_settings\" style=\"display:" + customDisplayStyle + ";\">";
  html += "<label>Broker Host:</label><input type=\"text\" name=\"mqtt_host\" value=\"" + htmlEscape(mqttBroker) + "\">";
  html += "<label>Broker Port:</label><input type=\"number\" min=\"1\" max=\"65535\" name=\"mqtt_port\" value=\"" + String(mqttPort) + "\">";
  html += "<label>MQTT Username:</label><input type=\"text\" name=\"mqtt_user\" value=\"" + htmlEscape(mqttUsername) + "\">";
  html += "<label>MQTT Password:</label><input type=\"password\" name=\"mqtt_pass\" value=\"" + htmlEscape(mqttPassword) + "\">";
  html += "</div></div>";

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
    html += "</script>";
  }

  html += "<script>toggleNetworkFields();updateGeneratedTopic();updateCsvEstimate();window.onclick=function(e){var m=document.getElementById('infoModal');if(e.target==m)closeInfo();};</script>";
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
  PortalDataState state = portalDataState();
  if (state.pendingRows > 0 && (!server.hasArg("discard_pending") || server.arg("discard_pending") != "1")) {
    messagePage("MQTT Upload Still Pending",
                String(state.pendingRows) + " record(s) have not been acknowledged. "
                "Return to the portal and use the confirmed discard option only after downloading the CSV.");
    return;
  }
  if (LittleFS.exists(csvFilename.c_str())) {
    LittleFS.remove(csvFilename.c_str());
  }
  clearMqttCursorState();
  bootCount = 0;
  prevStatus = -1;
  lastLoggedEpoch = 0;
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleNewDeployment() {
  PortalDataState state = portalDataState();
  if (state.fileSize > 0 || state.pendingRows > 0) {
    messagePage("Current Deployment Is Not Closed",
                "Download and delete the current CSV, and resolve any pending MQTT uploads, before starting a new deployment.");
    return;
  }
  deploymentID = deploymentID == UINT32_MAX ? 1 : deploymentID + 1;
  preferences.putUInt("deployment_id", deploymentID);
  bootCount = 0;
  prevStatus = -1;
  lastLoggedEpoch = 0;
  clearMqttCursorState();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleFileDownload() {
  String path = normalizeCsvPath(server.arg("file"));
  if (!server.hasArg("file") || !isSafeCsvPath(path) || path == csvFilename) {
    server.send(400, "text/plain", "Invalid recovered CSV path.");
    return;
  }
  File file = LittleFS.open(path.c_str(), FILE_READ);
  if (!file) {
    server.send(404, "text/plain", "Recovered CSV not found.");
    return;
  }
  String downloadName = path.substring(1);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  server.streamFile(file, "text/csv");
  file.close();
}

void handleFileDelete() {
  String path = normalizeCsvPath(server.arg("file"));
  if (!server.hasArg("file") || !isSafeCsvPath(path) || path == csvFilename) {
    server.send(400, "text/plain", "Invalid recovered CSV path.");
    return;
  }
  if (!LittleFS.exists(path.c_str())) {
    server.send(404, "text/plain", "Recovered CSV not found.");
    return;
  }
  LittleFS.remove(path.c_str());
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSave() {
  String savedSensorID = server.hasArg("id") ? server.arg("id") : sensorID;
  savedSensorID.trim();
  if (!isValidSensorID(savedSensorID)) {
    messagePage("Invalid Sensor ID",
                "Use 1-48 characters and do not use slashes, commas, quotes, angle brackets, or line breaks.", 400);
    return;
  }

  String savedPrefix = server.hasArg("mqtt_prefix") ? server.arg("mqtt_prefix") : mqttTopicPrefix;
  savedPrefix.trim();
  if (!isValidMqttPrefix(savedPrefix)) {
    messagePage("Invalid MQTT Topic Prefix",
                "Enter a non-empty root topic without spaces, wildcards, duplicate slashes, or leading/trailing slashes.", 400);
    return;
  }

  PortalDataState state = portalDataState();
  if (savedSensorID != sensorID && state.fileSize > 0) {
    messagePage("Download and Delete the Current CSV First",
                "Changing the Sensor ID now would hide <strong>" + htmlEscape(sensorID) +
                ".csv</strong> in device storage. Download and delete it before renaming this deployment.");
    return;
  }

  int savedSleep = server.hasArg("sleep") ? max(server.arg("sleep").toInt(), 1L) : sleepSeconds;
  int savedMaxLogInterval = server.hasArg("max_log_int")
                              ? max(server.arg("max_log_int").toInt(), 1L)
                              : maxLoggedIntervalMins;
  float savedThreshold = server.hasArg("rss_thresh") ? server.arg("rss_thresh").toFloat() : RSSsafety;
  int savedPowerSource = server.hasArg("power_source") ? server.arg("power_source").toInt() : (isUsbBankMode() ? 0 : 1);
  int savedNetworkMode = server.hasArg("network_mode") ? server.arg("network_mode").toInt() : networkUseMode();
  if (savedPowerSource < 0 || savedPowerSource > 1) savedPowerSource = 0;
  if (savedNetworkMode < 0 || savedNetworkMode > 2) savedNetworkMode = 0;
  bool savedWifiEnabled = savedNetworkMode != 0;
  int savedDeploymentMode = savedPowerSource == 0
                              ? (savedWifiEnabled ? USB_BANK_WIFI : USB_BANK_NO_WIFI)
                              : (savedWifiEnabled ? POWER_CUBE_WIFI : POWER_CUBE_NO_WIFI);
  int savedMqttMode = MQTT_DISABLED;
  if (savedNetworkMode == 2) {
    savedMqttMode = server.hasArg("mqtt_server") ? server.arg("mqtt_server").toInt() : MQTT_DEFAULT_SERVER;
    if (savedMqttMode != MQTT_DEFAULT_SERVER && savedMqttMode != MQTT_CUSTOM_SERVER) {
      savedMqttMode = MQTT_DEFAULT_SERVER;
    }
  }

  String savedBroker = server.hasArg("mqtt_host") ? server.arg("mqtt_host") : mqttBroker;
  savedBroker.trim();
  uint16_t savedPort = mqttPort;
  if (server.hasArg("mqtt_port")) {
    long candidatePort = server.arg("mqtt_port").toInt();
    if (candidatePort >= 1 && candidatePort <= 65535) savedPort = (uint16_t)candidatePort;
  }
  bool destinationChanged = savedMqttMode != mqttMode || savedPrefix != effectiveMqttTopicPrefix();
  if (savedMqttMode == MQTT_CUSTOM_SERVER || mqttMode == MQTT_CUSTOM_SERVER) {
    destinationChanged = destinationChanged || savedBroker != mqttBroker || savedPort != mqttPort;
  }
  if (state.pendingRows > 0 && destinationChanged) {
    messagePage("Resolve the MQTT Backlog First",
                String(state.pendingRows) + " record(s) are still assigned to the current MQTT destination. "
                "Resume the current deployment to finish uploading, or download and explicitly delete the CSV before changing the destination.");
    return;
  }

  preferences.putString("id", savedSensorID);
  preferences.putInt("sleep", savedSleep);
  preferences.putInt("max_log_int", savedMaxLogInterval);
  preferences.putFloat("rss_thresh", savedThreshold);
  preferences.putInt("deploy_mode", savedDeploymentMode);
  preferences.putInt("mqtt_mode", savedMqttMode);
  preferences.putBool("mqtt_awake", savedMqttMode != MQTT_DISABLED && savedPowerSource == 1);
  if (savedMqttMode == MQTT_DISABLED) {
    // Rows created while MQTT is deliberately disabled must not be uploaded
    // later if the user re-enables the same broker/topic configuration.
    preferences.remove("mqoff");
    preferences.remove("mqfile");
    preferences.remove("mqroute");
  }
  if (server.hasArg("wifi_ssid")) preferences.putString("wifi_ssid", server.arg("wifi_ssid"));
  if (server.hasArg("wifi_pass")) preferences.putString("wifi_pass", server.arg("wifi_pass"));
  preferences.remove("mqtt_id");
  preferences.putString("mqtt_host", savedBroker);
  preferences.putUInt("mqtt_port", savedPort);
  if (server.hasArg("mqtt_user")) preferences.putString("mqtt_user", server.arg("mqtt_user"));
  if (server.hasArg("mqtt_pass")) preferences.putString("mqtt_pass", server.arg("mqtt_pass"));
  preferences.putString("mqtt_prefix", savedPrefix);

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

  String downloadName = sensorID + "--" + deviceUID + "--d" + String(deploymentID) + ".csv";
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

String effectiveMqttTopicPrefix() {
  String value = mqttTopicPrefix;
  value.trim();
  while (value.startsWith("/")) value.remove(0, 1);
  while (value.endsWith("/")) value.remove(value.length() - 1);
  if (value.length() == 0) value = DEFAULT_MQTT_TOPIC_PREFIX;
  return value;
}

String mqttTopic() {
  return effectiveMqttTopicPrefix() + "/" + MQTT_FIXED_TOPIC_PATH + "/" +
         deviceUID + "/readings";
}

String mqttSafeField(String value) {
  value.trim();
  value.replace(",", "_");
  value.replace("\r", "_");
  value.replace("\n", "_");
  return value;
}

bool mqttWorkOutstanding() {
  return mqttWorkerPending || mqttWorkerActive ||
         (mqttQueue != nullptr && uxQueueMessagesWaiting(mqttQueue) > 0);
}

String mqttRouteIdentity() {
  String broker = mqttMode == MQTT_CUSTOM_SERVER ? mqttBroker : String(DEFAULT_MQTT_BROKER);
  uint16_t port = mqttMode == MQTT_CUSTOM_SERVER ? mqttPort : DEFAULT_MQTT_PORT;
  return broker + ":" + String(port) + "|" + mqttTopic();
}

uint32_t currentCsvSize() {
  File file = LittleFS.open(csvFilename.c_str(), FILE_READ);
  if (!file) return 0;
  uint32_t size = file.size();
  file.close();
  return size;
}

void persistMqttCursor(bool force) {
  if (!force && mqttRowsSinceCheckpoint < MQTT_CURSOR_CHECKPOINT_ROWS) return;
  preferences.putString("mqfile", csvFilename);
  preferences.putString("mqroute", mqttRouteIdentity());
  preferences.putUInt("mqoff", mqttCursorOffset);
  mqttCheckpointOffset = mqttCursorOffset;
  mqttRowsSinceCheckpoint = 0;
  Serial.printf("[MQTT] Saved CSV upload cursor at byte %lu.\n", (unsigned long)mqttCursorOffset);
}

void clearMqttCursorState() {
  mqttCursorOffset = 0;
  mqttCheckpointOffset = 0;
  mqttRowsSinceCheckpoint = 0;
  mqttBacklogPending = false;
  mqttRetryNotBeforeMs = 0;
  preferences.putString("mqfile", csvFilename);
  preferences.putString("mqroute", mqttRouteIdentity());
  preferences.putUInt("mqoff", 0);
}

void initializeMqttCursor() {
  if (!mqttEnabled()) return;
  uint32_t fileSize = currentCsvSize();
  String storedFile = preferences.getString("mqfile", "");
  String storedRoute = preferences.getString("mqroute", "");
  uint32_t storedOffset = preferences.getUInt("mqoff", 0);
  bool cursorMatches = preferences.isKey("mqoff") &&
                       storedFile == csvFilename &&
                       storedRoute == mqttRouteIdentity() &&
                       storedOffset <= fileSize;

  // A new MQTT route starts with new CSV rows. Existing rows are left in the
  // CSV but are not silently rerouted under a different broker/topic/ID.
  mqttCursorOffset = cursorMatches ? storedOffset : fileSize;
  mqttCheckpointOffset = mqttCursorOffset;
  mqttRowsSinceCheckpoint = 0;
  mqttRetryNotBeforeMs = 0;
  mqttBacklogPending = mqttCursorOffset < fileSize;
  persistMqttCursor(true);
  Serial.printf("[MQTT] CSV cursor initialized at %lu/%lu bytes; backlog=%s.\n",
                (unsigned long)mqttCursorOffset, (unsigned long)fileSize,
                mqttBacklogPending ? "yes" : "no");
}

bool parseCsvRow(const String& line, MqttCsvRow* row, uint32_t nextOffset) {
  int month, day, year, hour, minute, second, status;
  float rms;
  int fields = sscanf(line.c_str(), "%d/%d/%d %d:%d:%d,%d,%f",
                      &month, &day, &year, &hour, &minute, &second, &status, &rms);
  if (fields != 8 || status < 0 || status > 1 || month < 1 || month > 12 ||
      day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    return false;
  }

  struct tm timeinfo = {};
  timeinfo.tm_year = year - 1900;
  timeinfo.tm_mon = month - 1;
  timeinfo.tm_mday = day;
  timeinfo.tm_hour = hour;
  timeinfo.tm_min = minute;
  timeinfo.tm_sec = second;
  timeinfo.tm_isdst = -1;
  time_t epoch = mktime(&timeinfo);
  if (epoch <= 0) return false;

  row->epoch = epoch;
  row->status = status;
  row->rms = rms;
  row->nextOffset = nextOffset;
  return true;
}

bool readNextPendingCsvRow(MqttCsvRow* row) {
  File file = LittleFS.open(csvFilename.c_str(), FILE_READ);
  if (!file) {
    mqttBacklogPending = false;
    return false;
  }
  uint32_t fileSize = file.size();
  if (mqttCursorOffset > fileSize || !file.seek(mqttCursorOffset)) {
    file.close();
    Serial.println("[MQTT] CSV cursor is invalid; preserving CSV and stopping catch-up.");
    mqttBacklogPending = false;
    return false;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    uint32_t nextOffset = file.position();
    line.trim();
    if (line.length() == 0 || line.startsWith("Month/")) {
      mqttCursorOffset = nextOffset;
      continue;
    }
    if (parseCsvRow(line, row, nextOffset)) {
      file.close();
      return true;
    }
    Serial.printf("[MQTT] Skipping malformed CSV backlog row at byte %lu.\n",
                  (unsigned long)mqttCursorOffset);
    mqttCursorOffset = nextOffset;
    mqttRowsSinceCheckpoint++;
    persistMqttCursor(false);
  }

  mqttBacklogPending = mqttCursorOffset < fileSize;
  file.close();
  return false;
}

void refreshMqttBacklogState() {
  mqttBacklogPending = mqttEnabled() && mqttCursorOffset < currentCsvSize();
}

void mqttEventHandler(void* handlerArgs, esp_event_base_t eventBase,
                      int32_t eventId, void* eventData) {
  (void)handlerArgs;
  (void)eventBase;
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;
  if (eventId == MQTT_EVENT_CONNECTED) {
    mqttBrokerConnected = true;
  } else if (eventId == MQTT_EVENT_DISCONNECTED) {
    mqttBrokerConnected = false;
  } else if (eventId == MQTT_EVENT_PUBLISHED) {
    mqttLastAcknowledgedMessageId = event->msg_id;
  }
}

void stopMqttClient() {
  if (activeMqttClient != nullptr) {
    esp_mqtt_client_stop(activeMqttClient);
    esp_mqtt_client_destroy(activeMqttClient);
    activeMqttClient = nullptr;
  }
  mqttBrokerConnected = false;
  mqttLastAcknowledgedMessageId = -1;
}

bool connectMqtt() {
  if (wifiSSID.length() == 0) {
    Serial.println("[MQTT] Wi-Fi SSID is empty; publish skipped.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < MQTT_WIFI_TIMEOUT_MS) {
      delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[MQTT] Wi-Fi connection timed out; local CSV data is unchanged.");
      turnWifiOff();
      return false;
    }
  }

  String broker = mqttMode == MQTT_CUSTOM_SERVER ? mqttBroker : String(DEFAULT_MQTT_BROKER);
  uint16_t port = mqttMode == MQTT_CUSTOM_SERVER ? mqttPort : DEFAULT_MQTT_PORT;
  broker.trim();
  if (broker.length() == 0) {
    Serial.println("[MQTT] Broker host is empty; publish skipped.");
    return false;
  }

  if (activeMqttClient != nullptr && mqttBrokerConnected) return true;
  stopMqttClient();
  activeMqttBroker = broker;
  activeMqttUsername = mqttMode == MQTT_CUSTOM_SERVER ? mqttUsername : String(DEFAULT_MQTT_USERNAME);
  activeMqttPassword = mqttMode == MQTT_CUSTOM_SERVER ? mqttPassword : String(DEFAULT_MQTT_PASSWORD);
  activeMqttClientID = "onoff-client-" + deviceUID;

  esp_mqtt_client_config_t config = {};
  config.broker.address.hostname = activeMqttBroker.c_str();
  config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  config.broker.address.port = port;
  config.credentials.client_id = activeMqttClientID.c_str();
  config.credentials.username = activeMqttUsername.length() ? activeMqttUsername.c_str() : nullptr;
  config.credentials.authentication.password = activeMqttPassword.length() ? activeMqttPassword.c_str() : nullptr;
  config.session.keepalive = 30;
  config.network.timeout_ms = 3000;
  config.network.disable_auto_reconnect = true;
  config.task.stack_size = 6144;
  config.buffer.size = 512;

  activeMqttClient = esp_mqtt_client_init(&config);
  if (activeMqttClient == nullptr) {
    Serial.println("[MQTT] Failed to initialize QoS 1 client.");
    return false;
  }
  esp_mqtt_client_register_event(activeMqttClient, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
  mqttBrokerConnected = false;
  Serial.printf("[MQTT] Connecting to %s:%u with QoS 1.\n", activeMqttBroker.c_str(), port);
  if (esp_mqtt_client_start(activeMqttClient) != ESP_OK) {
    stopMqttClient();
    return false;
  }
  unsigned long started = millis();
  while (!mqttBrokerConnected && millis() - started < MQTT_WIFI_TIMEOUT_MS) delay(10);
  if (!mqttBrokerConnected) {
    Serial.println("[MQTT] Broker connection timed out; CSV backlog retained.");
    stopMqttClient();
    return false;
  }
  return true;
}

bool publishMqttCsvRow(const MqttCsvRow& measurement) {
  if (!connectMqtt()) return false;

  String topic = mqttTopic();
  String safeSensorID = mqttSafeField(sensorID);
  char payload[320];
  snprintf(payload, sizeof(payload), "%lld,%d,%.4f,%s,%lu,%s,%s,%s",
           (long long)measurement.epoch, measurement.status, measurement.rms,
           safeSensorID.c_str(), (unsigned long)deploymentID, deviceUID.c_str(),
           FIRMWARE_VERSION, BOARD_NAME);
  mqttLastAcknowledgedMessageId = -1;
  int messageId = esp_mqtt_client_publish(activeMqttClient, topic.c_str(), payload, 0, 1, 0);
  if (messageId < 0) {
    Serial.printf("[MQTT] QoS 1 enqueue failed (%d); CSV backlog retained.\n", messageId);
    return false;
  }
  unsigned long started = millis();
  while (mqttBrokerConnected && mqttLastAcknowledgedMessageId != messageId &&
         millis() - started < MQTT_ACK_TIMEOUT_MS) {
    delay(10);
  }
  bool acknowledged = mqttLastAcknowledgedMessageId == messageId;
  Serial.printf("[MQTT] %s id=%d topic=%s payload=%s\n",
                acknowledged ? "Broker acknowledged" : "Acknowledgment failed",
                messageId, topic.c_str(), payload);
  return acknowledged;
}

void mqttWorkerTask(void* parameter) {
  (void)parameter;
  uint8_t signal;

  for (;;) {
    if (measurementInProgress) {
      delay(10);
      continue;
    }

    if (xQueueReceive(mqttQueue, &signal, pdMS_TO_TICKS(100)) == pdTRUE) {
      mqttWorkerActive = true;
      mqttWorkerPending = false;
      if (isUsbBankMode()) setCpuFrequencyMhz(normalCpuFrequencyMhz);
      refreshMqttBacklogState();

      uint8_t uploaded = 0;
      bool batchFailed = false;
      while (mqttBacklogPending && uploaded < MQTT_CATCHUP_ROWS_PER_CYCLE) {
        MqttCsvRow row;
        if (!readNextPendingCsvRow(&row)) break;
        if (!publishMqttCsvRow(row)) {
          batchFailed = true;
          break;
        }
        mqttCursorOffset = row.nextOffset;
        mqttRowsSinceCheckpoint++;
        persistMqttCursor(false);
        uploaded++;
        refreshMqttBacklogState();
      }
      if (batchFailed) {
        mqttRetryNotBeforeMs = millis() + MQTT_RETRY_BACKOFF_MS;
        Serial.printf("[MQTT] Backlog retained; next retry in %lu seconds.\n",
                      (unsigned long)(MQTT_RETRY_BACKOFF_MS / 1000UL));
      } else if (uploaded > 0) {
        mqttRetryNotBeforeMs = 0;
      }

      if (!mqttStayAwake) {
        stopMqttClient();
        turnWifiOff();
        if (isUsbBankMode()) setCpuFrequencyMhz(USB_BASELINE_CPU_MHZ);
      }
      mqttWorkerActive = false;
      mqttWorkerPending = uxQueueMessagesWaiting(mqttQueue) > 0;
    }
  }
}

bool startMqttWorker() {
  if (!mqttEnabled()) return true;
  initializeMqttCursor();
  mqttQueue = xQueueCreate(1, sizeof(uint8_t));
  if (mqttQueue == nullptr) {
    Serial.println("[MQTT] Failed to create upload queue.");
    return false;
  }

#if CONFIG_IDF_TARGET_ESP32C3
  BaseType_t created = xTaskCreate(mqttWorkerTask, "OnOff-MQTT", 6144, nullptr, 1, &mqttTaskHandle);
#else
  BaseType_t created = xTaskCreatePinnedToCore(mqttWorkerTask, "OnOff-MQTT", 6144,
                                               nullptr, 1, &mqttTaskHandle, 0);
#endif
  if (created != pdPASS) {
    Serial.println("[MQTT] Failed to start upload worker.");
    vQueueDelete(mqttQueue);
    mqttQueue = nullptr;
    return false;
  }
#if CONFIG_IDF_TARGET_ESP32C3
  Serial.println("[MQTT] Cooperative single-core upload worker started.");
#else
  Serial.println("[MQTT] Upload worker started on the S3 network core.");
#endif
  refreshMqttBacklogState();
  return true;
}

bool queueMqttUpload() {
  if (!mqttEnabled() || mqttQueue == nullptr) return false;
  refreshMqttBacklogState();
  if (!mqttBacklogPending) return true;
  if (!mqttRetryDue()) return false;
  uint8_t signal = 1;
  mqttWorkerPending = true;
  if (xQueueSend(mqttQueue, &signal, 0) != pdTRUE) {
    mqttWorkerPending = uxQueueMessagesWaiting(mqttQueue) > 0;
    // A queued wake signal already covers every pending row in the CSV.
    return mqttWorkerPending;
  }
  return true;
}

bool waitForMqttCompletion(unsigned long timeoutMs) {
  unsigned long started = millis();
  while (mqttWorkOutstanding() && millis() - started < timeoutMs) smartDelay(10);
  if (mqttWorkOutstanding()) {
    Serial.println("[MQTT] Upload timed out; local acquisition will continue.");
    return false;
  }
  return true;
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
  server.on("/new-deployment", handleNewDeployment);
  server.on("/save", handleSave);
  server.on("/download", handleDownload);
  server.on("/file-download", handleFileDownload);
  server.on("/file-delete", handleFileDelete);
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

bool writeMeasurementIfNeeded(float rmsValue, time_t nowEpoch, bool* wroteRow) {
  *wroteRow = false;
  machineStatus = (rmsValue > RSSsafety) ? 1 : 0;
  Serial.printf("Calculated RMS: %.4f | Status: %d\n", rmsValue, machineStatus);

  bool stateChanged = machineStatus != prevStatus;
  bool maximumIntervalDue = lastLoggedEpoch == 0 ||
                            nowEpoch < lastLoggedEpoch ||
                            nowEpoch - lastLoggedEpoch >= (time_t)maxLoggedIntervalMins * 60;
  bool shouldWrite = stateChanged || maximumIntervalDue;

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
  *wroteRow = true;
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

  // Active station-mode Wi-Fi provides a stronger bank load than the validated
  // AP pulse. Do not switch radio modes in the middle of an MQTT transaction.
  if (mqttWorkOutstanding() || (mqttAlwaysConnected() && WiFi.status() == WL_CONNECTED)) {
    nextUsbKeepAliveUs = nowUs + (uint64_t)USB_KEEPALIVE_INTERVAL_MS * 1000ULL;
    return;
  }

  runUsbKeepAlivePulse();
  nowUs = esp_timer_get_time();
  do {
    nextUsbKeepAliveUs += (uint64_t)USB_KEEPALIVE_INTERVAL_MS * 1000ULL;
  } while (nextUsbKeepAliveUs <= nowUs);
}

void waitInUsbBankMode(unsigned long waitMs) {
  if (!mqttWorkOutstanding() && !mqttAlwaysConnected()) turnWifiOff();
  Serial.printf("[POWER] USB-bank wait at %u MHz for %lu ms; pulse=%lu/%lu ms.\n",
                USB_BASELINE_CPU_MHZ, waitMs,
                (unsigned long)USB_KEEPALIVE_DURATION_MS,
                (unsigned long)USB_KEEPALIVE_INTERVAL_MS);
  Serial.flush();
  if (!mqttWorkOutstanding() && !mqttAlwaysConnected()) setCpuFrequencyMhz(USB_BASELINE_CPU_MHZ);
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
  if (mqttWorkOutstanding()) waitForMqttCompletion(MQTT_WIFI_TIMEOUT_MS + 15000UL);
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
  mqttMode = preferences.getInt("mqtt_mode", MQTT_DISABLED);
  if (mqttMode < MQTT_DISABLED || mqttMode > MQTT_CUSTOM_SERVER) mqttMode = MQTT_DISABLED;
  if (mqttMode != MQTT_DISABLED) {
    if (deploymentMode == USB_BANK_NO_WIFI) deploymentMode = USB_BANK_WIFI;
    if (deploymentMode == POWER_CUBE_NO_WIFI) deploymentMode = POWER_CUBE_WIFI;
  }
  mqttStayAwake = mqttMode != MQTT_DISABLED && !isUsbBankMode();
  mqttBroker = preferences.getString("mqtt_host", DEFAULT_MQTT_BROKER);
  mqttPort = (uint16_t)preferences.getUInt("mqtt_port", DEFAULT_MQTT_PORT);
  if (mqttPort == 0) mqttPort = DEFAULT_MQTT_PORT;
  mqttUsername = preferences.getString("mqtt_user", DEFAULT_MQTT_USERNAME);
  mqttPassword = preferences.getString("mqtt_pass", DEFAULT_MQTT_PASSWORD);
  if (preferences.isKey("mqtt_prefix")) {
    mqttTopicPrefix = preferences.getString("mqtt_prefix", DEFAULT_MQTT_TOPIC_PREFIX);
  } else {
    String legacyPrefix = preferences.getString("mqtt_topic", DEFAULT_MQTT_TOPIC_PREFIX);
    mqttTopicPrefix = legacyPrefix == "emqx/onoff" ? String(DEFAULT_MQTT_TOPIC_PREFIX) : legacyPrefix;
    if (!isValidMqttPrefix(mqttTopicPrefix)) mqttTopicPrefix = DEFAULT_MQTT_TOPIC_PREFIX;
    preferences.putString("mqtt_prefix", mqttTopicPrefix);
  }
  preferences.remove("mqtt_id");
  deploymentID = max(preferences.getUInt("deployment_id", 1), (uint32_t)1);
  preferences.putUInt("deployment_id", deploymentID);
  deviceUID = readDeviceUID();
  csvFilename = "/" + sensorID + ".csv";
  normalCpuFrequencyMhz = getCpuFrequencyMhz();
  Serial.printf("[V6] version=%s board=%s uid=%s deployment=%lu normal_cpu_mhz=%lu status_led=%s\n",
                FIRMWARE_VERSION, BOARD_NAME, deviceUID.c_str(), (unsigned long)deploymentID,
                (unsigned long)normalCpuFrequencyMhz, HAS_STATUS_LED ? "yes" : "no");

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
  startMqttWorker();
  if (isUsbBankMode()) {
    nextUsbKeepAliveUs = esp_timer_get_time() + (uint64_t)USB_KEEPALIVE_INTERVAL_MS * 1000ULL;
  }
  Serial.printf("[MODE] deployment=%d usb_bank=%s wifi_recovery=%s mqtt=%d mqtt_awake=%s\n",
                deploymentMode,
                isUsbBankMode() ? "yes" : "no",
                wifiRecoveryEnabled() ? "yes" : "no",
                mqttMode,
                mqttStayAwake ? "yes" : "no");
  Serial.printf("[TIMING] setup_ready_ms=%lu\n", millis() - setupStartMs);
}

void loop() {
  if (mqttWorkOutstanding()) waitForMqttCompletion(MQTT_WIFI_TIMEOUT_MS + 15000UL);
  prepareUsbBankMeasurement();
  unsigned long cycleStartMs = millis();
  bootCount++;
  Serial.printf("Boot/Cycle Count: %d\n", bootCount);
  blinkLED(2, 200);

  float rmsValue = 0.0;
  measurementInProgress = true;
  if (!collectMeasurement(&rmsValue)) {
    measurementInProgress = false;
    Serial.println("[SENSOR ERROR] Measurement discarded; no OFF row will be written.");
  } else {
    measurementInProgress = false;
    time_t nowEpoch;
    time(&nowEpoch);
    bool wroteRow = false;
    writeMeasurementIfNeeded(rmsValue, nowEpoch, &wroteRow);
    if (mqttEnabled() && (wroteRow || mqttBacklogPending)) queueMqttUpload();
  }

  unsigned long activeMs = millis() - cycleStartMs;
  if (isUsbBankMode()) {
    unsigned long waitMs = remainingIntervalMs(activeMs, 0);
    Serial.printf("[TIMING] active_ms=%lu wait_ms=%lu target_ms=%lu\n",
                  activeMs, waitMs, (unsigned long)sleepSeconds * 1000UL);
    waitInUsbBankMode(waitMs);
    return;
  }

  if (mqttEnabled() && !mqttStayAwake) {
    waitForMqttCompletion(MQTT_WIFI_TIMEOUT_MS + 15000UL);
    activeMs = millis() - cycleStartMs;
  }

  if (mqttEnabled() && mqttStayAwake) {
    unsigned long waitMs = remainingIntervalMs(activeMs, 0);
    Serial.printf("[POWER] MQTT always-connected awake wait for %lu ms.\n", waitMs);
    smartDelay(waitMs);
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
