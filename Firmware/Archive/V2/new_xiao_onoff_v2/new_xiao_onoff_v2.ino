#include <WiFi.h>
#include <Wire.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>

// AP Mode Configuration
#define AP_SSID "OnOff"
#define AP_PASS "onoff123"

#define sampleCount 1000

// --- CUSTOM PCB MAPPING ---
#define I2C_SDA 5   
#define I2C_SCL 6   
#define LED_PIN 21    
#define BUTTON_PIN 0  

// Time Configuration (EST/EDT for New Jersey)
const char* tzInfo = "EST5EDT,M3.2.0,M11.1.0"; 
const char* ntpServer = "pool.ntp.org";

// --- Variables ---
Preferences preferences;
String sensorID;
String csvFilename; 
int sleepSeconds;
int maxLoggedIntervalMins; 
float RSSsafety; // Dynamic threshold loaded from preferences
bool isRunning = false;
bool exitPortal = false; 
bool flashMounted = false;  

// Hybrid Time Recovery Variables
int syncMode = 0; // 0 = Phone (Air-gapped), 1 = WiFi Recovery
String wifiSSID;
String wifiPass;

float instAccelXYZ[3];
int machineStatus = 0;

// DEEP SLEEP RETENTION VARIABLES
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int prevStatus = -1;       
RTC_DATA_ATTR bool timeSynced = false;   
RTC_DATA_ATTR bool distressMode = false; // <-- CRITICAL: Retains error state across deep sleep

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

// --- Smart Delay (Allows button to interrupt loops) ---
void smartDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("\n[INTERRUPT] Button pressed! Halting sensor and rebooting...");
      preferences.putBool("running", false);
      for(int i=0; i<10; i++) {
        digitalWrite(LED_PIN, LOW); delay(50);
        digitalWrite(LED_PIN, HIGH); delay(50);
      }
      ESP.restart(); 
    }
    delay(10); 
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
  html += "button{color: white; padding: 15px 32px; font-size: 18px; border: none; border-radius: 8px; cursor: pointer; width: 100%; margin-top: 10px; transition: 0.3s;}";
  html += ".btn-start{background-color: #28a745;} .btn-start:hover:enabled{background-color: #218838;}";
  html += ".btn-stop{background-color: #dc3545;} .btn-stop:hover{background-color: #c82333;}";
  html += ".btn-dl{background-color: #007bff;} .btn-dl:hover{background-color: #0056b3;}";
  html += ".btn-clear{background-color: #ff9800;} .btn-clear:hover{background-color: #e68a00;}";
  html += "input, select{width: 100%; padding: 10px; margin: 8px 0; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box;}</style>";
  
  // Dynamic UI Script
  html += "<script>";
  html += "function toggleWiFiFields() {";
  html += "  var mode = document.getElementById('sync_mode').value;";
  html += "  var wifiDiv = document.getElementById('wifi_settings');";
  html += "  if(mode == '1') { wifiDiv.style.display = 'block'; } else { wifiDiv.style.display = 'none'; }";
  html += "}";
  html += "</script></head>";
  
  html += "<body><div class=\"card\"><h2>XIAO Sensor Portal</h2>";
  html += "<p><strong>Sensor ID:</strong> " + sensorID + "</p>";
  
  if (isRunning) {
    html += "<h3 style=\"color:#28a745;\">Status: RUNNING</h3>";
    html += "<p style=\"font-size:12px; color:#666;\">(Will resume sampling when portal closes)</p>";
  } else {
    html += "<h3 style=\"color:#dc3545;\">Status: IDLE</h3>";
    html += "<p id=\"sync-status\" style=\"color:#ff9800; font-weight:bold;\">🕒 Syncing backup time from phone...</p>";
  }

  if (!flashMounted) {
    html += "<div style=\"background-color:#ffcccc; padding:10px; border-radius:8px; margin-bottom:15px;\">";
    html += "<h3 style=\"color:#dc3545; margin:0;\">⚠️ Memory Error</h3>";
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
    html += "<a href=\"/clear\" onclick=\"return confirm('Are you sure you want to PERMANENTLY delete all data on the internal flash?');\"><button class=\"btn-clear\">Wipe Flash Data</button></a>";
  }
  
  // Configuration Form 
  html += "<hr style=\"margin: 25px 0;\"><h3 style=\"text-align:left;\">Sensor Configuration</h3>";
  html += "<form action=\"/save\" method=\"POST\">";
  html += "<div style=\"text-align: left;\">";
  html += "<label>Sensor ID (CSV Filename):</label><br><input type=\"text\" name=\"id\" value=\"" + sensorID + "\"><br>";
  html += "<label>Sampling Wake/Sleep Interval (Sec):</label><br><input type=\"number\" name=\"sleep\" value=\"" + String(sleepSeconds) + "\"><br>";
  html += "<label>Maximum Logged Interval (Mins):</label><br><input type=\"number\" name=\"max_log_int\" value=\"" + String(maxLoggedIntervalMins) + "\"><br>";
  
  // Threshold Configuration
  html += "<label>Vibration Threshold (RMS):</label><br>";
  html += "<p style=\"font-size:12px; color:#666; margin-top:2px; margin-bottom:4px;\"><em>(Default: 0.028 | Lowest recorded: 0.0068. Check CSV baseline to avoid false ONs from background noise.)</em></p>";
  html += "<input type=\"number\" step=\"0.0001\" name=\"rss_thresh\" value=\"" + String(RSSsafety, 4) + "\"><br>";

  // Recovery Mode Selector
  html += "<label>Power-Loss Recovery Mode:</label><br>";
  html += "<select id=\"sync_mode\" name=\"sync_mode\" onchange=\"toggleWiFiFields()\">";
  html += "<option value=\"0\" " + String(syncMode == 0 ? "selected" : "") + ">Phone (Air-Gapped / Safe Lockout)</option>";
  html += "<option value=\"1\" " + String(syncMode == 1 ? "selected" : "") + ">WiFi (Auto-Recovery)</option>";
  html += "</select><br>";
  
  // Conditional WiFi Fields
  String displayStyle = syncMode == 1 ? "block" : "none";
  html += "<div id=\"wifi_settings\" style=\"display:" + displayStyle + "; background-color:#e9ecef; padding:10px; border-radius:5px; margin-top:10px;\">";
  html += "<p style=\"font-size:12px; margin-top:0;\"><strong>Note:</strong> Used ONLY for auto-recovering time if the battery dies. If WiFi fails, it falls back to requiring a Phone sync.</p>";
  html += "<label>WiFi SSID:</label><input type=\"text\" name=\"wifi_ssid\" value=\"" + wifiSSID + "\">";
  html += "<label>WiFi Password:</label><input type=\"password\" name=\"wifi_pass\" value=\"" + wifiPass + "\">";
  html += "</div>";
  
  html += "</div>";
  html += "<button type=\"submit\" class=\"btn-dl\" style=\"background-color:#6c757d; margin-top:15px;\">Save Settings</button>";
  html += "</form>";
  html += "</div>";

  // JAVASCRIPT: Auto-Sync Phone Time to ESP32 (The Just-In-Case Fallback)
  if (!isRunning) {
    html += "<script>";
    html += "window.onload = function() {";
    html += "  var ts = Math.floor(Date.now() / 1000);"; 
    html += "  fetch('/settime?ts=' + ts).then(function(response) {";
    html += "    if(response.ok) {";
    html += "      var st = document.getElementById('sync-status');";
    html += "      st.innerHTML = '✅ Time Synced to Phone';";
    html += "      st.style.color = '#28a745';";
    html += "      var btn = document.getElementById('btn-start');";
    html += "      if(btn) { btn.disabled = false; btn.style.opacity = 1; }"; 
    html += "    }";
    html += "  });";
    html += "};";
    html += "toggleWiFiFields();"; // Run once to ensure correct UI state
    html += "</script>";
  }
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSetTime() {
  if (server.hasArg("ts")) {
    long unix_time = server.arg("ts").toInt();
    
    struct timeval tv;
    tv.tv_sec = unix_time;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    
    timeSynced = true;
    distressMode = false; // <-- Clear distress state once phone provides the time
    Serial.printf("Time successfully synced from browser fallback: %ld\n", unix_time);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing timestamp");
  }
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
  if (LittleFS.exists(csvFilename.c_str())) {
    LittleFS.remove(csvFilename.c_str());
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
  
  if (server.hasArg("rss_thresh")) preferences.putFloat("rss_thresh", server.arg("rss_thresh").toFloat());
  
  if (server.hasArg("sync_mode")) preferences.putInt("sync_mode", server.arg("sync_mode").toInt());
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

// --- Main Logic ---
void setup() {
  Serial.begin(115200);
  
  setenv("TZ", tzInfo, 1);
  tzset();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Active LOW
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(100); 

  // Initialize Internal Flash Memory (LittleFS)
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed! Check Partition Scheme.");
    flashMounted = false;
    blinkLED(20, 50); 
  } else {
    Serial.println("LittleFS Mount Successful.");
    flashMounted = true;
  }

  // Load preferences
  preferences.begin("config", false);
  sensorID = preferences.getString("id", "OnOffSensor");
  sleepSeconds = preferences.getInt("sleep", 10); 
  maxLoggedIntervalMins = preferences.getInt("max_log_int", 5); 
  RSSsafety = preferences.getFloat("rss_thresh", 0.028); // Load dynamic threshold
  isRunning = preferences.getBool("running", false);
  syncMode = preferences.getInt("sync_mode", 0);
  wifiSSID = preferences.getString("wifi_ssid", "");
  wifiPass = preferences.getString("wifi_pass", "");

  csvFilename = "/" + sensorID + ".csv";

  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  bool openPortal = false;
  bool emergencyAlarm = false;

  // Double check time validity if waking from deep sleep
  struct tm timeCheck;
  if (!getLocalTime(&timeCheck) || timeCheck.tm_year <= 100) {
      timeSynced = false;
  } else {
      timeSynced = true;
      distressMode = false; // Clock is fine, clear distress just in case
  }

  // ===== WAKE LOGIC =====
  if (digitalRead(BUTTON_PIN) == LOW || wakeReason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Button Wake/Hold Detected! Opening Portal.");
    openPortal = true;
    distressMode = false; // Manual intervention clears distress flag
    while(digitalRead(BUTTON_PIN) == LOW) { delay(10); } 
  } 
  else if (wakeReason != ESP_SLEEP_WAKEUP_TIMER) {
    if (isRunning) {
      Serial.println("Power recovered! But time is lost.");
    } else {
      Serial.println("Clean boot (Idle). Opening portal...");
      openPortal = true;
    }
  }
  
  // =========================================================
  // HYBRID POWER-LOSS AUTO RECOVERY LOGIC
  // If sensor lost power while RUNNING, it attempts recovery.
  // =========================================================
  if (isRunning && !timeSynced && !openPortal) {
      Serial.println("CRITICAL: Time lost due to power failure.");
      
      if (syncMode == 1 && wifiSSID.length() > 0) {
          Serial.println("Mode: WiFi Auto-Recovery. Connecting just to fetch time...");
          WiFi.mode(WIFI_STA);
          WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
          
          int attempts = 0;
          // Wait max 15 seconds to connect
          while (WiFi.status() != WL_CONNECTED && attempts < 15) {
              delay(1000);
              Serial.print(".");
              attempts++;
          }
          
          if (WiFi.status() == WL_CONNECTED) {
              Serial.println("\nWiFi Connected! Requesting NTP...");
              configTzTime(tzInfo, ntpServer);
              
              // Wait max 10 seconds for NTP payload
              if (getLocalTime(&timeCheck, 10000) && timeCheck.tm_year > 100) {
                  timeSynced = true;
                  distressMode = false;
                  Serial.println("NTP Sync Successful! Resuming data collection instantly.");
              } else {
                  Serial.println("NTP Server timeout.");
              }
          } else {
              Serial.println("\nWiFi Connection Failed (Network down?).");
          }
          WiFi.disconnect(true);
      }
      
      // If recovery failed (or if set to Air-Gapped mode), fail safely to portal
      if (!timeSynced) {
          Serial.println("Time Recovery Failed. Triggering VISUAL ALARM and forcing portal.");
          isRunning = false;
          preferences.putBool("running", false);
          openPortal = true;
          emergencyAlarm = true;
          distressMode = true; // <-- Enter persistent distress state
      }
  }

  // ===== MODE 1: AP / Configuration Portal =====
  if (openPortal) { 
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
    // CRITICAL FIX: Do NOT keep portal open infinitely on alarm. Let it timeout to save battery.
    while ((millis() - portalStartTime < 180000) && !exitPortal) {
      server.handleClient();
      
      if (emergencyAlarm || distressMode) {
        // Aggressive double-strobe every second to alert nearby workers
        int cycle = millis() % 1000;
        if (cycle < 100 || (cycle > 200 && cycle < 300)) {
          digitalWrite(LED_PIN, LOW); // ON
        } else {
          digitalWrite(LED_PIN, HIGH); // OFF
        }
      } else {
        // Normal setup mode: simple slow blink
        digitalWrite(LED_PIN, (millis() / 500) % 2 == 0 ? LOW : HIGH);
      }
      
      delay(10); 
    }
    
    server.stop();
    WiFi.softAPdisconnect(true);
    digitalWrite(LED_PIN, HIGH); // Turn LED off
    
    // Update isRunning in case it was toggled in the portal
    isRunning = preferences.getBool("running", false);
  }

  // ===== MODE 2: Normal Sensing Mode =====
  if (isRunning && timeSynced) {
    bootCount++;
    Serial.printf("Boot Count: %d\n", bootCount);
    blinkLED(2, 200); 
    
    gyro_init();
    
    // =========================================================
    // SENSOR WARM-UP & TRANSIENT FLUSH
    // Let the MPU stabilize from a cold boot to prevent the 
    // High-Pass Filter from seeing a fake 1g "shock" spike.
    // =========================================================
    delay(100);
    for(int i = 0; i < 20; i++) {
      gyro_signals(); 
      delay(5);
    }

    float alpha = RC / (RC + (dT / 1000.0));
    float sumSq = 0.0;
    
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

    // Determine Maximum Logged Interval Math (+2 Sec offset)
    int bootsPerMaxLog = (maxLoggedIntervalMins * 60) / (sleepSeconds + 2);
    if (bootsPerMaxLog < 1) bootsPerMaxLog = 1;

    bool shouldwrite = false;
    if (machineStatus != prevStatus) {
      shouldwrite = true; 
    } else if (bootCount == 1 || bootCount % bootsPerMaxLog == 0) {
      shouldwrite = true; 
    }

    if (flashMounted && shouldwrite) {
      bool fileExists = LittleFS.exists(csvFilename.c_str());
      File file = LittleFS.open(csvFilename.c_str(), FILE_APPEND);
      if (file) {
        if (!fileExists) file.println("Month/DD/YYYY HH:MM, Status, RSS value");
        file.printf("%s,%d,%.4f\n", timeString, machineStatus, RMS);
        file.close();
        Serial.println("Data saved to Flash.");
      }
    } else if (!shouldwrite) {
      Serial.println("Skipping flash write - No state change.");
    }

    prevStatus = machineStatus;
    
    Serial.println("Going to sleep now.");
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  } else {
    // We are NOT running normally. Fall back to Beacon or pure IDLE sleep.
    if (distressMode) {
      // LOW POWER BEACON: Blip the LED twice quickly to show distress without draining battery
      digitalWrite(LED_PIN, LOW); delay(30); digitalWrite(LED_PIN, HIGH); delay(100);
      digitalWrite(LED_PIN, LOW); delay(30); digitalWrite(LED_PIN, HIGH);
      Serial.println("Distress Mode Active. Sleeping for 4 seconds...");
      esp_sleep_enable_timer_wakeup(4000000ULL); // Wake up every 4 seconds to blink
    } else {
      Serial.println("System IDLE. Going to deep sleep until button press.");
      // No timer wakeup, sleeps forever
    }
    Serial.flush();
  }
  
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0); 
  esp_deep_sleep_start();
}

void loop() {}