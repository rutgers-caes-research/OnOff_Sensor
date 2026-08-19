#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <SdFat.h> 
#include <FS.h>
#include <WebServer.h>
#include <time.h>
#include <Preferences.h>
#include "driver/rtc_io.h" 

// AP Mode Configuration
#define AP_SSID "XiaoSensor"
#define AP_PASS "12345678"

#define sampleCount 1000

/*
// --- Online Server Upload Configuration (Commented out for future use) ---
const char* serverName = "34.63.139.91";
const int serverPort = 80;
const String serverPath = "/cgi-bin/upload.py";
*/

// --- CUSTOM PCB SPI MAPPING ---
#define SD_CS   44
#define SD_MOSI 9
#define SD_MISO 8
#define SD_SCLK 7
#define SD_SPEED_MHZ 1 

#define I2C_SDA 5   
#define I2C_SCL 6   
#define LED_PIN 21    
#define BUTTON_PIN 0  

// Time Configuration (EST/EDT for New Jersey)
const char* ntpServer = "pool.ntp.org";
const char* tzInfo = "EST5EDT,M3.2.0,M11.1.0"; 

// --- Variables ---
Preferences preferences;
String sensorID;
String wifiSSID;
String wifiPASS;
String csvFilename; // Dynamically set based on sensorID
int sleepSeconds;
int maxLoggedIntervalMins; // Max time between data collection saves
bool isRunning = false;
bool exitPortal = false; 
bool sdMounted = false;  

SdFat SD;

float instAccelXYZ[3];
float RSSsafety = 0.028; 
int machineStatus = 0;

// DEEP SLEEP RETENTION VARIABLES
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int prevStatus = -1;       // Used for shouldwrite logic
RTC_DATA_ATTR bool timeSynced = false;   // Prevents the 1970 timestamp glitch

// Filter and sample tracking
float RC = 0.1; 
float dT = 1.0; 
static float xyzHPF[3][sampleCount];
static float Accel[3][sampleCount];

WebServer server(80);

// --- Status Light Helpers ---
void blinkLED(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW); // Active LOW
    delay(durationMs);
    digitalWrite(LED_PIN, HIGH);
    delay(durationMs);
  }
}

// --- Smart Delay (Allows button to interrupt infinite WiFi loops) ---
void smartDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("\n[INTERRUPT] Button pressed! Halting sensor and rebooting...");
      preferences.putBool("running", false);
      
      // Rapid flash to acknowledge button press
      for(int i=0; i<10; i++) {
        digitalWrite(LED_PIN, LOW); delay(50);
        digitalWrite(LED_PIN, HIGH); delay(50);
      }
      ESP.restart(); // Reboot to clear WiFi tasks and allow clean portal entry
    }
    delay(10); // Short yield to prevent watchdog resets
  }
}

// --- Sensor Functions ---
void gyro_signals() {
  Wire.beginTransmission(0x68);
  Wire.write(0x3B); 
  Wire.endTransmission(false);
  Wire.requestFrom((uint16_t)0x68, (uint8_t)6, true);

  int16_t rawX = Wire.read() << 8 | Wire.read();
  int16_t rawY = Wire.read() << 8 | Wire.read();
  int16_t rawZ = Wire.read() << 8 | Wire.read();

  // Convert raw values to standard gravity (g)
  instAccelXYZ[0] = (float)rawX / 16384.0;
  instAccelXYZ[1] = (float)rawY / 16384.0;
  instAccelXYZ[2] = (float)rawZ / 16384.0;
}

void gyro_init() {
  Wire.begin(I2C_SDA, I2C_SCL); 
  Wire.setClock(400000); 
  Wire.beginTransmission(0x68);
  Wire.write(0x6B); 
  Wire.write(0x00); 
  Wire.endTransmission(true);
}

// --- Web Server Functions (AP Mode) ---
void handleRoot() {
  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<style>body{font-family: Arial; text-align: center; margin-top: 30px; background-color: #f4f6f9; color: #333;}";
  html += ".card{max-width: 400px; margin: 0 auto; background: white; padding: 30px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.1);}";
  html += "button{color: white; padding: 15px 32px; font-size: 18px; border: none; border-radius: 8px; cursor: pointer; width: 100%; margin-top: 10px;}";
  html += ".btn-start{background-color: #28a745;} .btn-start:hover{background-color: #218838;}";
  html += ".btn-stop{background-color: #dc3545;} .btn-stop:hover{background-color: #c82333;}";
  html += ".btn-dl{background-color: #007bff;} .btn-dl:hover{background-color: #0056b3;}";
  html += ".btn-clear{background-color: #ff9800;} .btn-clear:hover{background-color: #e68a00;}";
  html += "input{width: 100%; padding: 10px; margin: 8px 0; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box;}</style></head>";
  html += "<body><div class=\"card\"><h2>XIAO Sensor Portal</h2>";
  html += "<p><strong>Sensor ID:</strong> " + sensorID + "</p>";
  
  if (isRunning) {
    html += "<h3 style=\"color:#28a745;\">Status: RUNNING</h3>";
    html += "<p style=\"font-size:12px; color:#666;\">(Will resume sampling when portal closes)</p>";
  } else {
    html += "<h3 style=\"color:#dc3545;\">Status: IDLE</h3>";
  }

  // --- HARDWARE DIAGNOSTIC UI ---
  if (!sdMounted) {
    html += "<div style=\"background-color:#ffcccc; padding:10px; border-radius:8px; margin-bottom:15px;\">";
    html += "<h3 style=\"color:#dc3545; margin:0;\">⚠️ SD Card Error</h3>";
    html += "<p style=\"color:#dc3545; font-size:14px; margin-top:5px;\">SdFat mount failed. Please check physical SD card.</p></div>";
  } else {
    FsFile file = SD.open(csvFilename.c_str(), O_READ);
    if (file) {
      html += "<p><strong>Data file size:</strong> " + String(file.size() / 1024.0, 2) + " KB</p>";
      file.close();
    } else {
      html += "<p style=\"color:#ff9800; font-weight:bold;\">No data file found yet. (Click 'Start' to begin logging!)</p>";
    }
  }

  if (!isRunning) {
    html += "<a href=\"/start\"><button class=\"btn-start\">Start Data Collection</button></a>";
  } else {
    html += "<a href=\"/stop\"><button class=\"btn-stop\">Stop / Idle Mode</button></a>";
  }
  
  html += "<a href=\"/download\"><button class=\"btn-dl\">Download CSV</button></a>";
  
  if (!isRunning) {
    html += "<a href=\"/clear\" onclick=\"return confirm('Are you sure you want to PERMANENTLY delete all data on the SD card?');\"><button class=\"btn-clear\">Wipe SD Card Data</button></a>";
  }
  
  // Configuration Form
  html += "<hr style=\"margin: 25px 0;\"><h3 style=\"text-align:left;\">Sensor Configuration</h3>";
  html += "<form action=\"/save\" method=\"POST\">";
  html += "<div style=\"text-align: left;\">";
  html += "<label>Sensor ID (Used for CSV Filename):</label><br><input type=\"text\" name=\"id\" value=\"" + sensorID + "\"><br>";
  html += "<label>Sampling Wake/Sleep Interval (Sec) - Time it sleeps between readings:</label><br><input type=\"number\" name=\"sleep\" value=\"" + String(sleepSeconds) + "\"><br>";
  html += "<label>Maximum Logged Interval (Mins) - Max time between data collection saves:</label><br><input type=\"number\" name=\"max_log_int\" value=\"" + String(maxLoggedIntervalMins) + "\"><br>";
  html += "<label>TimeSync WiFi SSID:</label><br><input type=\"text\" name=\"ssid\" value=\"" + wifiSSID + "\"><br>";
  html += "<label>TimeSync WiFi Password:</label><br><input type=\"password\" name=\"pass\" value=\"" + wifiPASS + "\"></div>";
  html += "<button type=\"submit\" class=\"btn-dl\" style=\"background-color:#6c757d;\">Save Settings</button>";
  html += "</form>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleStart() {
  preferences.putBool("running", true);
  isRunning = true;
  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><style>body{font-family: Arial; text-align: center; margin-top: 50px;}</style></head><body><h2>Collection Started!</h2><p>Portal closing. Sensor is entering deep sleep sampling cycles.</p></body></html>";
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
  if (SD.exists(csvFilename.c_str())) {
    SD.remove(csvFilename.c_str());
  }
  bootCount = 0; 
  prevStatus = -1;
  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><style>body{font-family: Arial; text-align: center; margin-top: 50px;}</style></head><body><h2>Data Cleared!</h2><p>Redirecting back to portal...</p></body></html>";
  server.send(200, "text/html", html);
  delay(1000);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSave() {
  if (server.hasArg("id")) preferences.putString("id", server.arg("id"));
  if (server.hasArg("sleep")) preferences.putInt("sleep", server.arg("sleep").toInt());
  if (server.hasArg("max_log_int")) preferences.putInt("max_log_int", server.arg("max_log_int").toInt());
  if (server.hasArg("ssid")) preferences.putString("ssid", server.arg("ssid"));
  if (server.hasArg("pass")) preferences.putString("pass", server.arg("pass"));
  
  String html = "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><style>body{font-family: Arial; text-align: center; margin-top: 50px;}</style></head><body><h2>Settings Saved!</h2><p>Rebooting sensor to apply...</p></body></html>";
  server.send(200, "text/html", html);
  delay(1000);
  ESP.restart(); 
}

void handleDownload() {
  FsFile file = SD.open(csvFilename.c_str(), O_READ);
  if (!file) {
    server.send(404, "text/plain", "File not found on SD card.");
    return;
  }
  
  String downloadName = sensorID + ".csv";
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  server.sendHeader("Connection", "close");
  server.setContentLength(file.size());
  server.send(200, "text/csv", ""); 

  WiFiClient client = server.client();
  uint8_t buffer[512];
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    client.write(buffer, bytesRead);
  }
  file.close();
}

// --- Network Functions ---

/*
// --- Online Server Upload Logic (Commented out for future use) ---
void uploadFileHttpFromSD(String filepath) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Skipping upload.");
    return;
  }

  FsFile file = SD.open(filepath.c_str(), O_READ);
  if (!file) {
    Serial.println("Failed to open file for uploading");
    return;
  }

  WiFiClient client;
  if (!client.connect(serverName, serverPort)) {
    Serial.println("Connection to server failed");
    file.close();
    return;
  }

  String boundary = "----ESP32Boundary" + String(millis());
  String head = "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filepath.substring(1) + "\"\r\n";
  head += "Content-Type: text/csv\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t totalLength = head.length() + file.size() + tail.length();

  client.println("POST " + serverPath + " HTTP/1.1");
  client.println("Host: " + String(serverName));
  client.println("Content-Length: " + String(totalLength));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println();
  client.print(head);

  uint8_t buffer[512];
  while (file.available()) {
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    client.write(buffer, bytesRead);
  }
  
  client.print(tail);
  file.close();

  Serial.println("Upload complete.");
  client.stop();
}
*/

bool connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(wifiSSID);
  WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { 
    smartDelay(500); // Interruptible wait
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  return (WiFi.status() == WL_CONNECTED);
}

void syncNTPTime() {
  if (connectWiFi()) {
    Serial.println("Syncing time via NTP...");
    configTzTime(tzInfo, ntpServer);
    
    struct tm timeinfo;
    int retry = 0;
    
    // Explicitly wait for the background NTP process to fetch a valid year (>2000)
    while (retry < 20) {
      if (getLocalTime(&timeinfo, 1000)) {
        if (timeinfo.tm_year > 100) { // Year is > 2000
          timeSynced = true;
          Serial.println("\nNTP Time successfully synced!");
          break;
        }
      }
      Serial.print("*");
      smartDelay(10); // Quick check to keep button responsive
      retry++;
    }
    
    if (!timeSynced) {
      Serial.println("\nFailed to sync time (NTP timeout).");
    }
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100); // Give the power rails time to completely stabilize after WiFi turns off
  } else {
    Serial.println("WiFi connection failed.");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

// --- Main Logic ---
void setup() {
  Serial.begin(115200);
  
  // =========================================================
  // TIMEZONE DEEP SLEEP FIX
  // =========================================================
  setenv("TZ", tzInfo, 1);
  tzset();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); 
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(100); 
  
  // =========================================================
  // RTC GPIO RELEASE: Crucial for ESP32-S3 Deep Sleep wakeups
  // =========================================================
  rtc_gpio_deinit((gpio_num_t)SD_SCLK);
  rtc_gpio_deinit((gpio_num_t)SD_MISO);
  rtc_gpio_deinit((gpio_num_t)SD_MOSI);
  
  gpio_hold_dis((gpio_num_t)SD_SCLK);
  gpio_hold_dis((gpio_num_t)SD_MISO);
  gpio_hold_dis((gpio_num_t)SD_MOSI);
  gpio_hold_dis((gpio_num_t)SD_CS);

  // --- SD CARD INITIALIZATION ---
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  
  if (!SD.begin(SD_CS, SD_SCK_MHZ(SD_SPEED_MHZ))) {
    Serial.println("SdFat Mount Failed!");
    sdMounted = false;
    // PHYSICAL WARNING: Rapidly flash LED 20 times if SD card is missing/failed
    blinkLED(20, 50); 
  } else {
    Serial.println("SdFat Mount Successful.");
    sdMounted = true;
  }

  // Load saved configurations from memory
  preferences.begin("config", false);
  sensorID = preferences.getString("id", "XiaoSensor");
  wifiSSID = preferences.getString("ssid", "ezamp");
  wifiPASS = preferences.getString("pass", "ezamp123");
  sleepSeconds = preferences.getInt("sleep", 10); 
  maxLoggedIntervalMins = preferences.getInt("max_log_int", 5); 
  isRunning = preferences.getBool("running", false);

  // Dynamic Filename Generation
  csvFilename = "/" + sensorID + ".csv";

  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  bool openPortal = false;

  // ===== WAKE LOGIC =====
  if (digitalRead(BUTTON_PIN) == LOW || wakeReason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Button Wake/Hold Detected!");
    openPortal = true;
    while(digitalRead(BUTTON_PIN) == LOW) { delay(10); } 
  } 
  else if (wakeReason != ESP_SLEEP_WAKEUP_TIMER) {
    if (isRunning) {
      Serial.println("Power recovered! Resuming collection automatically...");
      timeSynced = false; // Force a fresh sync on unexpected reboot
    } else {
      Serial.println("Clean boot (Idle). Opening portal...");
      openPortal = true;
    }
  }

  // Double check time validity if waking from deep sleep
  struct tm timeCheck;
  if (!getLocalTime(&timeCheck) || timeCheck.tm_year <= 100) {
      timeSynced = false;
  }

  // ===== MODE 1: AP / Configuration Portal =====
  if (openPortal) {
    Serial.println("\n--- Entering AP Portal Mode ---");
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
    
    server.on("/", handleRoot);
    server.on("/start", handleStart);
    server.on("/stop", handleStop);
    server.on("/clear", handleClear);
    server.on("/save", handleSave);
    server.on("/download", handleDownload);
    server.begin();
    
    Serial.println("Web Server Started. Portal will remain open for 3 minutes.");
    
    unsigned long portalStartTime = millis();
    while (millis() - portalStartTime < 180000 && !exitPortal) {
      server.handleClient();
      digitalWrite(LED_PIN, (millis() / 150) % 2 == 0 ? LOW : HIGH); 
      delay(10); 
    }
    
    server.stop();
    WiFi.softAPdisconnect(true);
    digitalWrite(LED_PIN, HIGH); 
    
    isRunning = preferences.getBool("running", false);
  }

  // ===== MODE 2: Normal Sensing Mode =====
  if (isRunning) {
    bootCount++;
    Serial.printf("Boot Count: %d\n", bootCount);
    blinkLED(2, 200); 

    // =========================================================
    // STRICT TIMESTAMPS: Infinitely loop until time is acquired!
    // =========================================================
    while (!timeSynced) {
       Serial.println("\n[WARNING] Time is lost/invalid! Enforcing strict NTP Sync...");
       syncNTPTime();
       
       if (!timeSynced) {
          Serial.println("Time sync failed. Retrying in 5 seconds...");
          blinkLED(5, 100); 
          smartDelay(4000); // Interruptible wait
       }
    }
    
    gyro_init();
    
    // =========================================================
    // FIX: SENSOR WARM-UP & TRANSIENT FLUSH
    // Let the MPU stabilize from a cold boot to prevent the 
    // High-Pass Filter from seeing a fake 1g "shock" spike.
    // =========================================================
    delay(100);
    for(int i = 0; i < 20; i++) {
      gyro_signals(); // Throwaway the initial garbage readings
      delay(5);
    }

    float alpha = RC / (RC + (dT / 1000.0));
    float sumSq = 0.0;

    Serial.println("Sampling sensor data...");
    
    for (int i = 0; i < sampleCount; i++) {
      gyro_signals();
      
      for (int axis = 0; axis < 3; axis++) {
        Accel[axis][i] = instAccelXYZ[axis];
        if (i == 0) {
          xyzHPF[axis][i] = 0.0; 
        } else {
          xyzHPF[axis][i] = alpha * (xyzHPF[axis][i-1] + Accel[axis][i] - Accel[axis][i-1]);
        }
      }
      
      float mag = sqrt(pow(xyzHPF[0][i], 2) + pow(xyzHPF[1][i], 2) + pow(xyzHPF[2][i], 2));
      sumSq += (mag * mag);
      delay(dT);
    }

    float RMS = sqrt(sumSq / sampleCount);
    machineStatus = (RMS > RSSsafety) ? 1 : 0;
    
    Serial.printf("Calculated RMS: %.4f | Status: %d\n", RMS, machineStatus);

    struct tm timeinfo;
    char timeString[64];
    if (getLocalTime(&timeinfo)) {
      strftime(timeString, sizeof(timeString), "%m/%d/%Y %H:%M:%S", &timeinfo);
    } else {
      strcpy(timeString, "01/01/1970 00:00:00"); 
    }

    // Determine Maximum Logged Interval Math
    // ADDED +2 SECONDS: The chip is awake for ~2 seconds (sampling, writing, I2C)
    // Without this, the 10-second sleep math leads to 12-second physical cycles (6 mins instead of 5)
    int bootsPerMaxLog = (maxLoggedIntervalMins * 60) / (sleepSeconds + 2);
    if (bootsPerMaxLog < 1) bootsPerMaxLog = 1;

    // THE 'SHOULD WRITE' LOGIC RESTORED
    bool shouldwrite = false;
    if (machineStatus != prevStatus) {
      shouldwrite = true; // State changed (Machine turned on or off)
    } else if (bootCount == 1 || bootCount % bootsPerMaxLog == 0) {
      shouldwrite = true; // Periodic logging to show sensor is alive
    }

    if (sdMounted && shouldwrite) {
      bool fileExists = SD.exists(csvFilename.c_str());
      FsFile file = SD.open(csvFilename.c_str(), O_WRITE | O_CREAT | O_APPEND);
      if (file) {
        if (!fileExists) file.println("Month/DD/YYYY HH:MM, Status, RSS value");
        file.printf("%s,%d,%.4f\n", timeString, machineStatus, RMS);
        file.close();
        Serial.println("Data saved to SD (Change or Max Interval Reached).");
      } else {
        Serial.println("Failed to open CSV file for appending.");
      }
    } else if (!shouldwrite) {
      Serial.println("Skipping SD write - No state change.");
    }

    /*
    // --- Upload to Server (Online Feature - Currently Commented Out) ---
    if (bootCount % bootsPerMaxLog == 0) {
      Serial.println("Upload interval reached. Connecting to WiFi...");
      connectWiFi();
      if (WiFi.status() == WL_CONNECTED) {
        uploadFileHttpFromSD(csvFilename);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      }
    } else {
      Serial.printf("Next upload in %d cycles.\n", bootsPerMaxLog - (bootCount % bootsPerMaxLog));
    }
    */

    // Update tracking variable for next deep sleep cycle
    prevStatus = machineStatus;

    Serial.println("Going to sleep now.");
    Serial.flush();
    
    if(sdMounted) {
        SD.end();
        SPI.end();
    }
    
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  } 
  else {
    Serial.println("System IDLE. Going to deep sleep until button press.");
    Serial.flush();
  }
  
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0); 
  esp_deep_sleep_start();
}

void loop() {
}