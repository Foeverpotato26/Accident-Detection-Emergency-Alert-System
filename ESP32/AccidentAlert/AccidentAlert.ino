/*
 * Accident Detection & Emergency Alert System
 * ESP32 DevKit V1 + MPU6050/MPU6500 + NEO-6M GPS + SOS button + 2 LEDs
 * Data is sent to Firebase Realtime Database over the REST API.
 *
 * Setup: copy config.example.h to config.h and enter your own values.
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <MPU6500_WE.h>
#include <TinyGPSPlus.h>
#include <math.h>
#include "config.h"

// ================= PINS =================
#define SDA_PIN       21
#define SCL_PIN       22
#define GPS_RX        16   // ESP32 RX2  <- GPS TX
#define GPS_TX        17   // ESP32 TX2  -> GPS RX
#define LED_GREEN     25
#define LED_RED       26
#define SOS_BUTTON    27

// ================= THRESHOLDS (calibrate for your prototype) =================
#define ACCEL_THRESHOLD 30.0   // m/s2  (1 g = 9.81)
#define GYRO_THRESHOLD  300.0  // deg/s

// ================= TIMERS =================
const unsigned long UPLOAD_INTERVAL     = 3000;
const unsigned long PRINT_INTERVAL      = 1000;
const unsigned long WIFI_RETRY_INTERVAL = 15000;
const unsigned long ALERT_DURATION      = 10000;
const unsigned long GPS_MAX_AGE_MS      = 10000;      // fix older than this = no fix
const int           MAX_FAILURES_BEFORE_WIFI_RESET = 10;

// ================= OBJECTS =================
MPU6500_WE mpu = MPU6500_WE(0x68);
TinyGPSPlus gps;
HardwareSerial GPSserial(2);

// ================= VARIABLES =================
bool mpuReady = false;
bool accidentDetected = false;
bool manualSOS = false;
bool previousButtonState = HIGH;
bool pendingHistory = false;   // true -> log the next ALERT in /history

float acceleration = 0, rotation = 0, temperature = 0;
double latitude = 0, longitude = 0;
int satellites = 0;

unsigned long lastUpload = 0, lastPrint = 0, lastWiFiRetry = 0, alertStartTime = 0;
int consecutiveFailures = 0;

String alertType = "NORMAL";

// ================= FUNCTION DECLARATIONS =================
void connectWiFi();
void maintainWiFi();
void readGPS();
bool gpsHasFix();
void readMotionSensor();
void checkSOSButton();
void activateAlert(String type);
void checkAlertTimeout();
void updateLEDs();
String buildJson();
bool firebaseRequest(const char* method, const String& path, const String& body);
void sendToFirebase();
void printDiagnostics();
void handleSerialCommands();
void printHelp();

// ==================================================
// SETUP
// ==================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("======================================");
  Serial.println("     ESP32 ACCIDENT ALERT SYSTEM");
  Serial.println("======================================");

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  pinMode(SOS_BUTTON, INPUT_PULLUP);
  Serial.println("[OK] LEDs configured");
  Serial.println("[OK] SOS button configured");

  Serial.println();
  Serial.println("[MPU] Initializing I2C...");
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(200);

  if (mpu.init()) {
    mpuReady = true;
    Serial.println("[OK] Motion sensor detected (I2C 0x68)");
    mpu.setAccRange(MPU6500_ACC_RANGE_16G);
    mpu.setGyrRange(MPU6500_GYRO_RANGE_500);
    mpu.enableAccDLPF(true);
    mpu.setAccDLPF(MPU6500_DLPF_6);
    Serial.println("[OK] Accelerometer and gyroscope configured");
  } else {
    mpuReady = false;
    Serial.println("[ERROR] Motion sensor NOT DETECTED! Check SDA, SCL, VCC and GND.");
  }

  Serial.println();
  Serial.println("[GPS] Initializing GPS module...");
  GPSserial.setRxBufferSize(1024);   // large buffer: HTTPS uploads block the loop for a while
  GPSserial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("[OK] GPS Serial2 started (RX=GPIO16, TX=GPIO17)");

  Serial.println();
  connectWiFi();
  updateLEDs();

  Serial.println();
  Serial.println("SYSTEM INITIALIZATION FINISHED");
  printHelp();
}

// ==================================================
// LOOP
// ==================================================
void loop() {
  readGPS();
  if (mpuReady) readMotionSensor();
  checkSOSButton();
  checkAlertTimeout();
  handleSerialCommands();
  maintainWiFi();
  updateLEDs();

  if (millis() - lastPrint >= PRINT_INTERVAL) {
    lastPrint = millis();
    printDiagnostics();
  }

  if (millis() - lastUpload >= UPLOAD_INTERVAL) {
    lastUpload = millis();
    sendToFirebase();
  }
}

// ==================================================
// WIFI
// ==================================================
void connectWiFi() {
  Serial.println("[WIFI] Connecting...");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] Already connected");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[OK] Wi-Fi connected");
    Serial.print("[WIFI] IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] Signal strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("[ERROR] Wi-Fi connection timeout (will keep retrying)");
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiRetry >= WIFI_RETRY_INTERVAL) {
    lastWiFiRetry = millis();
    Serial.println("[WIFI] Disconnected - trying reconnection...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// ==================================================
// GPS
// ==================================================
bool gpsHasFix() {
  return gps.location.isValid() && gps.location.age() < GPS_MAX_AGE_MS;
}

void readGPS() {
  while (GPSserial.available()) {
    gps.encode(GPSserial.read());
  }
  if (gpsHasFix()) {
    latitude = gps.location.lat();
    longitude = gps.location.lng();
  }
  satellites = gps.satellites.isValid() ? gps.satellites.value() : 0;
}

// ==================================================
// MOTION SENSOR (acceleration in m/s2, rotation in deg/s)
// ==================================================
void readMotionSensor() {
  xyzFloat acc = mpu.getGValues();
  xyzFloat gyr = mpu.getGyrValues();
  temperature = mpu.getTemperature();

  float ax = acc.x * 9.80665;
  float ay = acc.y * 9.80665;
  float az = acc.z * 9.80665;
  acceleration = sqrt(ax * ax + ay * ay + az * az);
  rotation = sqrt(gyr.x * gyr.x + gyr.y * gyr.y + gyr.z * gyr.z);

  if (!accidentDetected && !manualSOS) {
    if (acceleration > ACCEL_THRESHOLD) {
      activateAlert("POSSIBLE_ACCIDENT_ACCELERATION");
    } else if (rotation > GYRO_THRESHOLD) {
      activateAlert("POSSIBLE_ACCIDENT_ROTATION");
    }
  }
}

// ==================================================
// SOS BUTTON (momentary: alert lasts while pressed)
// ==================================================
void checkSOSButton() {
  bool buttonState = digitalRead(SOS_BUTTON);

  if (previousButtonState == HIGH && buttonState == LOW) {
    delay(30);                                   // simple debounce
    if (digitalRead(SOS_BUTTON) != LOW) return;  // bounce / noise, ignore
    Serial.println();
    Serial.println("[SOS] MANUAL SOS ACTIVATED!");
    manualSOS = true;
    if (!accidentDetected) alertType = "MANUAL_SOS";
    pendingHistory = true;
    updateLEDs();
    lastUpload = millis() - UPLOAD_INTERVAL;
  }

  if (previousButtonState == LOW && buttonState == HIGH) {
    Serial.println();
    Serial.println("[SOS] BUTTON RELEASED!");
    manualSOS = false;
    if (!accidentDetected) {
      alertType = "NORMAL";
      lastUpload = millis() - UPLOAD_INTERVAL;
    }
    updateLEDs();
  }
  previousButtonState = buttonState;
}

// ==================================================
// AUTOMATIC ALERT
// ==================================================
void activateAlert(String type) {
  if (accidentDetected) return;

  accidentDetected = true;
  alertType = type;
  alertStartTime = millis();
  pendingHistory = true;

  Serial.println();
  Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  Serial.println("       ACCIDENT DETECTED!");
  Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  Serial.print("[ALERT] Type: ");
  Serial.println(alertType);

  updateLEDs();
  lastUpload = millis() - UPLOAD_INTERVAL;
}

void checkAlertTimeout() {
  if (accidentDetected && !manualSOS) {
    if (millis() - alertStartTime >= ALERT_DURATION) {
      accidentDetected = false;
      alertType = "NORMAL";
      Serial.println();
      Serial.println("[SYSTEM] Alert timeout reached - returning to NORMAL");
      updateLEDs();
      lastUpload = millis() - UPLOAD_INTERVAL;
    }
  }
}

void updateLEDs() {
  if (accidentDetected || manualSOS) {
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, HIGH);
  } else {
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, LOW);
  }
}

// ==================================================
// DIAGNOSTICS
// ==================================================
void printDiagnostics() {
  Serial.println();
  Serial.println("------------- LIVE STATUS -------------");
  Serial.print("[SYSTEM] Status: ");
  Serial.println((accidentDetected || manualSOS) ? "ALERT" : "NORMAL");
  Serial.print("[SYSTEM] Alert type: ");
  Serial.println(alertType);

  Serial.println();
  Serial.println("[MOTION SENSOR]");
  if (mpuReady) {
    Serial.print("Acceleration: "); Serial.print(acceleration); Serial.println(" m/s2");
    Serial.print("Rotation: ");     Serial.print(rotation);     Serial.println(" deg/s");
    Serial.print("Temperature: ");  Serial.print(temperature);  Serial.println(" C");
  } else {
    Serial.println("Sensor not detected");
  }

  Serial.println();
  Serial.println("[GPS]");
  Serial.print("Characters received: ");
  Serial.println(gps.charsProcessed());
  if (gpsHasFix()) {
    Serial.print("Latitude: ");   Serial.println(latitude, 6);
    Serial.print("Longitude: ");  Serial.println(longitude, 6);
    Serial.print("Satellites: "); Serial.println(satellites);
    Serial.print("Google Maps: https://www.google.com/maps?q=");
    Serial.print(latitude, 6); Serial.print(","); Serial.println(longitude, 6);
  } else {
    Serial.print("Waiting for GPS fix... satellites seen: ");
    Serial.println(satellites);
  }

  Serial.println();
  Serial.println("[WIFI]");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Disconnected");
  }

  Serial.println();
  Serial.println("[SOS]");
  Serial.println(digitalRead(SOS_BUTTON) == LOW ? "Button is pressed" : "Button is released");
  Serial.println("---------------------------------------");
}

// ==================================================
// FIREBASE (REST API)
// ==================================================
String buildJson() {
  bool fix = gpsHasFix();
  String latStr = fix ? String(latitude, 6) : String("null");
  String lonStr = fix ? String(longitude, 6) : String("null");
  char buf[420];
  snprintf(buf, sizeof(buf),
           "{\"status\":\"%s\",\"alertType\":\"%s\",\"acceleration\":%.2f,\"rotation\":%.2f,"
           "\"temperature\":%.2f,\"gpsValid\":%s,\"latitude\":%s,\"longitude\":%s,"
           "\"satellites\":%d,\"uptime\":%lu,\"updatedAt\":{\".sv\":\"timestamp\"}}",
           (accidentDetected || manualSOS) ? "ALERT" : "NORMAL",
           alertType.c_str(), acceleration, rotation, temperature,
           fix ? "true" : "false",
           latStr.c_str(), lonStr.c_str(),
           satellites, millis() / 1000);
  return String(buf);
}

bool firebaseRequest(const char* method, const String& path, const String& body) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[FIREBASE] Wi-Fi disconnected - upload skipped");
    return false;
  }

  String base = String(FIREBASE_URL);
  while (base.endsWith("/")) base.remove(base.length() - 1);   // avoid '//' in the URL
  String url = base + path + ".json";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.setTimeout(6000);
  if (!https.begin(client, url)) {
    Serial.println("[FIREBASE ERROR] HTTPS initialization failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");

  int code = https.sendRequest(method, body);
  Serial.print("[FIREBASE] "); Serial.print(method); Serial.print(" ");
  Serial.print(path); Serial.print(" -> HTTP "); Serial.println(code);

  bool ok = (code == HTTP_CODE_OK);
  if (code > 0 && !ok) {
    Serial.print("[FIREBASE] Response: ");
    Serial.println(https.getString());
    if (code == 401 || code == 403) Serial.println("[FIREBASE] Permission denied - check the database rules");
  } else if (code <= 0) {
    Serial.print("[FIREBASE ERROR] ");
    Serial.println(https.errorToString(code));
  }
  https.end();
  return ok;
}

void sendToFirebase() {
  String json = buildJson();
  bool ok = firebaseRequest("PUT", "/accidentAlert/current", json);

  // Log every new alert once in /accidentAlert/history
  if (ok && pendingHistory && (accidentDetected || manualSOS)) {
    if (firebaseRequest("POST", "/accidentAlert/history", json)) pendingHistory = false;
  } else if (!(accidentDetected || manualSOS)) {
    pendingHistory = false;
  }

  if (ok) {
    consecutiveFailures = 0;
  } else if (++consecutiveFailures >= MAX_FAILURES_BEFORE_WIFI_RESET) {
    Serial.println("[FIREBASE] Too many failures - resetting Wi-Fi");
    consecutiveFailures = 0;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// ==================================================
// SERIAL COMMANDS
// ==================================================
void handleSerialCommands() {
  if (!Serial.available()) return;
  char command = Serial.read();

  switch (command) {
    case 'h': case 'H': printHelp(); break;
    case 'i': case 'I': printDiagnostics(); break;
    case 'p': case 'P':
      Serial.print("Acceleration: "); Serial.println(acceleration);
      Serial.print("Rotation: ");     Serial.println(rotation);
      break;
    case 'w': case 'W':
      Serial.println("[WIFI] Manual reconnection requested");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      break;
    case 'n': case 'N': case 'd': case 'D':
      Serial.println("[TEST] Sending Firebase data now");
      sendToFirebase();
      break;
    case 'f': case 'F':
      Serial.println(String("[TEST] Firebase URL: ") + FIREBASE_URL + "/accidentAlert/current.json");
      break;
    case 't': case 'T':
      Serial.println("[TEST] Activating test accident alert");
      activateAlert("TEST_ALERT");
      break;
    case 'r': case 'R':
      Serial.println("[RESET] Restarting ESP32...");
      delay(1000);
      ESP.restart();
      break;
    case 'g': case 'G':
      if (gpsHasFix()) {
        Serial.print("Latitude: ");  Serial.println(latitude, 6);
        Serial.print("Longitude: "); Serial.println(longitude, 6);
      } else {
        Serial.println("No GPS fix yet");
      }
      break;
    case 'c': case 'C':
      accidentDetected = false;
      manualSOS = false;
      alertType = "NORMAL";
      updateLEDs();
      lastUpload = millis() - UPLOAD_INTERVAL;
      Serial.println("[RESET] Alert cleared");
      break;
    default: break;
  }
}

void printHelp() {
  Serial.println();
  Serial.println("========== SERIAL COMMANDS ==========");
  Serial.println("h = Show commands");
  Serial.println("i = Print complete diagnostics");
  Serial.println("p = Motion sensor values");
  Serial.println("w = Reconnect Wi-Fi");
  Serial.println("n / d = Send Firebase data now");
  Serial.println("f = Show Firebase URL");
  Serial.println("t = Test accident alert");
  Serial.println("r = Restart ESP32");
  Serial.println("g = Show GPS information");
  Serial.println("c = Clear alert");
  Serial.println("=====================================");
}
