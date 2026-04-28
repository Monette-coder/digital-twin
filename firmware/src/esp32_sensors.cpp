#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "DHT.h"

/*
  ESP32 + DHT11 + pH Sensor + WiFi Dashboard

  Wiring:
  - DHT11 DATA      -> GPIO4
  - pH sensor AO    -> GPIO35 (ADC1_7)
  - DHT11 VCC       -> 3.3V
  - pH sensor VCC   -> 3.3V
  - GND (both)      -> GND

  Sends readings to Flask server: 192.168.0.107:5000/api/sensor-data
*/

// ─── WiFi credentials ───────────────────────────────────────
const char* WIFI_SSID     = "Tenda_A92B00";
const char* WIFI_PASSWORD = "W4SeK*ho";

// ─── Flask server address ───────────────────────────────────
const char* SERVER_URL = "http://192.168.0.107:5000/api/sensor-data/esp32";

// ─── Pin definitions ────────────────────────────────────────
const uint8_t DHT_PIN = 4;           // GPIO4 for DHT11
const uint8_t PH_SENSOR_PIN = 35;    // GPIO35 (ADC1_7) for pH sensor

// ─── DHT11 sensor setup ─────────────────────────────────────
#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

// ─── pH Sensor calibration ──────────────────────────────────
// These values depend on your specific pH sensor
// You'll need to calibrate by measuring known pH solutions (pH 4.0, 7.0, 10.0)
const float PH_MIN = 0.0;           // pH value at analog min (0V)
const float PH_MAX = 14.0;          // pH value at analog max (3.3V)
const int ADC_MIN = 0;              // ADC reading at 0V
const int ADC_MAX = 4095;           // ADC reading at 3.3V (12-bit)

// ─── Reading interval ───────────────────────────────────────
const unsigned long READ_INTERVAL_MS = 5000;  // 5 seconds

unsigned long lastReadMs = 0;

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

float readPH() {
  // Read analog value from pH sensor
  int adcValue = analogRead(PH_SENSOR_PIN);
  
  // DEBUG: Print raw ADC value every reading
  Serial.print(" [ADC: ");
  Serial.print(adcValue);
  Serial.print("]");
  
  // Convert ADC value to pH (linear scaling)
  // For better accuracy, implement two-point calibration
  float ph = PH_MIN + (float)(adcValue - ADC_MIN) * (PH_MAX - PH_MIN) / (ADC_MAX - ADC_MIN);
  
  // Clamp to valid pH range
  ph = constrain(ph, 0.0, 14.0);
  
  return ph;
}

void sendToServer(float temp, float hum, float ph, int phRaw) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  WiFiClient wifiClient;
  HTTPClient http;

  http.begin(wifiClient, SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  // Format JSON payload (mimics soil sensor structure but with temp/humidity/pH)
  String json = "{";
  json += "\"soil_moisture\":0,";      // Not used in this setup
  json += "\"soil_percent\":0,";       // Not used in this setup
  json += "\"temperature\":" + String(temp, 2) + ",";
  json += "\"humidity\":" + String(hum, 2) + ",";
  json += "\"ph_value\":" + String(ph, 2) + ",";
  json += "\"ph_raw\":" + String(phRaw) + ",";
  json += "\"pump_status\":false";
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

  Serial.println();
  Serial.println("ESP32 Sensor Module (DHT11 + pH) starting...");
  Serial.println("DEBUG: Testing GPIO35 ADC before initialization...");
  
  // Quick ADC test before initialization
  int testRead = analogRead(35);
  Serial.print("Raw ADC test (default): ");
  Serial.println(testRead);

  // Initialize DHT sensor
  dht.begin();
  delay(500);

  // Set ADC resolution to 12-bit (0-4095) and full voltage range attenuation
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // 11dB attenuation for 0-3.3V range
  
  // Test ADC again after configuration
  testRead = analogRead(35);
  Serial.print("Raw ADC test (after config): ");
  Serial.println(testRead);

  connectWiFi();

  Serial.println("Sensor module initialized.");
  Serial.println("Reading DHT11 and pH sensor every " + String(READ_INTERVAL_MS) + "ms");
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

  // Read DHT11 (temperature & humidity)
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  // Check if DHT read failed
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("DHT read failed!");
    return;
  }

  // Read pH sensor
  int phRaw = analogRead(PH_SENSOR_PIN);
  float ph = readPH();

  // Print to serial
  Serial.print("Temp: ");
  Serial.print(temperature);
  Serial.print("°C | Humidity: ");
  Serial.print(humidity);
  Serial.print("% | pH: ");
  Serial.print(ph, 2);
  Serial.print(" (raw: ");
  Serial.print(phRaw);
  Serial.print(")");

  sendToServer(temperature, humidity, ph, phRaw);
  Serial.println();
}
