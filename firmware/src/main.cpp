#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

/*
  NodeMCU ESP8266 + Soil Sensor + Pump/Relay + WiFi Dashboard

  Suggested wiring:
  - Soil sensor AO  -> A0
  - Soil sensor VCC -> 3V3
  - Soil sensor GND -> GND
  - Relay IN        -> D1
  - Relay VCC       -> VIN/5V or 3V3 (depends on your relay module)
  - Relay GND       -> GND

  IMPORTANT:
  - Do NOT connect the pump directly to the ESP8266 pin.
  - Use a relay module or MOSFET driver and a separate pump power supply.
  - Make sure the grounds are common.
*/

// ─── WiFi credentials ───────────────────────────────────────────────
const char* WIFI_SSID     = "Tenda_A92B00";
const char* WIFI_PASSWORD = "W4SeK*ho";

// ─── Flask server address (your PC's local IP, e.g. 192.168.1.100) ─
const char* SERVER_URL = "http://192.168.0.107:5000/api/sensor-data";

// ─── Pin definitions ────────────────────────────────────────────────
const uint8_t SOIL_SENSOR_PIN = A0;
const uint8_t PUMP_RELAY_PIN  = D1;

// Many relay modules are active LOW. Change to false if yours is active HIGH.
const bool RELAY_ACTIVE_LOW = true;

// ─── Sensor calibration ─────────────────────────────────────────────
// Submerge sensor fully in water and note the value => WET_VALUE
// Leave sensor in open air and note the value      => DRY_VALUE
const int WET_VALUE = 600;
const int DRY_VALUE = 1024;

// Pump turns on when moisture % drops below this (0% = dry, 100% = wet)
const int DRY_THRESHOLD_PCT = 20;
const int HYSTERESIS_PCT    = 5;

const unsigned long READ_INTERVAL_MS = 3000;

bool pumpOn = false;
unsigned long lastReadMs = 0;

void setPump(bool turnOn) {
  pumpOn = turnOn;

  if (RELAY_ACTIVE_LOW) {
    digitalWrite(PUMP_RELAY_PIN, turnOn ? LOW : HIGH);
  } else {
    digitalWrite(PUMP_RELAY_PIN, turnOn ? HIGH : LOW);
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi connection failed. Running in offline mode.");
  }
}

int soilToPercent(int raw) {
  // Map raw sensor value to 0-100% (0% = dry, 100% = fully wet)
  int pct = map(constrain(raw, WET_VALUE, DRY_VALUE), DRY_VALUE, WET_VALUE, 0, 100);
  return constrain(pct, 0, 100);
}

void sendToServer(int soilRaw, int soilPct) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  WiFiClient wifiClient;
  HTTPClient http;

  http.begin(wifiClient, SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"soil_moisture\":" + String(soilRaw) + ",";
  json += "\"soil_percent\":" + String(soilPct) + ",";
  json += "\"temperature\":0,";
  json += "\"humidity\":0,";
  json += "\"pump_status\":" + String(pumpOn ? "true" : "false");
  json += "}";

  int httpCode = http.POST(json);

  if (httpCode > 0) {
    Serial.print(" | Sent to server (");
    Serial.print(httpCode);
    Serial.print(")");
  } else {
    Serial.print(" | Server send failed: ");
    Serial.print(http.errorToString(httpCode));
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PUMP_RELAY_PIN, OUTPUT);
  setPump(false);

  connectWiFi();

  Serial.println();
  Serial.println("Soil moisture pump controller started.");
  Serial.println("Watch the sensor values and adjust DRY_THRESHOLD if needed.");
}

void loop() {
  if (millis() - lastReadMs < READ_INTERVAL_MS) {
    return;
  }

  lastReadMs = millis();

  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  int soilRaw = analogRead(SOIL_SENSOR_PIN);
  int soilPct = soilToPercent(soilRaw);

  // Pump on when soil is too dry (low %), off when moist enough
  if (!pumpOn && soilPct <= (DRY_THRESHOLD_PCT - HYSTERESIS_PCT)) {
    setPump(true);
  } else if (pumpOn && soilPct >= (DRY_THRESHOLD_PCT + HYSTERESIS_PCT)) {
    setPump(false);
  }

  Serial.print("Soil raw: ");
  Serial.print(soilRaw);
  Serial.print(" | Moisture: ");
  Serial.print(soilPct);
  Serial.print("% | Pump: ");
  Serial.print(pumpOn ? "ON" : "OFF");

  sendToServer(soilRaw, soilPct);
  Serial.println();
}