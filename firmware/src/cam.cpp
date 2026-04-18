#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "esp_http_server.h"

/*
  ESP32-CAM (AI Thinker) – Plant Camera for Mons IoT

  This module provides:
    1. MJPEG live stream at http://<cam-ip>:81/stream
    2. Single JPEG snapshot at http://<cam-ip>/capture
    3. Periodic image upload to the Flask server

  Uses esp_http_server (native ESP-IDF) for reliable MJPEG streaming.
*/

// ─── WiFi credentials (same network as ESP8266 + Flask) ─────
const char* WIFI_SSID     = "Tenda_A92B00";
const char* WIFI_PASSWORD = "W4SeK*ho";

// ─── Flask server URL ───────────────────────────────────────
const char* SERVER_BASE   = "http://192.168.0.107:5000";

// ─── How often to upload a snapshot (ms) ────────────────────
const unsigned long CAPTURE_INTERVAL_MS = 18000000;  // every 5 hours

// ─── AI Thinker ESP32-CAM pin definitions ───────────────────
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Built-in flashlight LED
#define FLASH_LED_PIN      4

// HTTP server handles (native ESP-IDF httpd)
httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

unsigned long lastCaptureMs = 0;
unsigned long lastRegisterMs = 0;
const unsigned long REGISTER_INTERVAL_MS = 60000;

// ─── Camera init ────────────────────────────────────────────
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  // Optimized for smooth WiFi streaming
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_QVGA;  // 320x240
    config.jpeg_quality = 20;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 22;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  // Adjust sensor settings for better plant photos
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 1);
    s->set_saturation(s, 1);
  }

  // Discard first few frames (OV2640 returns black frames initially)
  for (int i = 0; i < 5; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(100);
  }

  Serial.println("Camera initialized OK");
  return true;
}

// ─── WiFi connection ────────────────────────────────────────
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
    Serial.print("Connected! Camera IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed!");
  }
}

// ─── Register camera with Flask server ──────────────────────
void registerWithServer() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(SERVER_BASE) + "/api/camera/register";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String ip = WiFi.localIP().toString();
  String json = "{\"ip\":\"" + ip + "\",\"stream_url\":\"http://" + ip + ":81/stream\"}";

  int code = http.POST(json);
  if (code == 200 || code == 201) {
    Serial.println("Registered with Flask server");
  } else {
    Serial.printf("Registration failed: %d\n", code);
  }
  http.end();
}

// ─── Upload a single JPEG to Flask ─────────────────────────
void uploadSnapshot() {
  if (WiFi.status() != WL_CONNECTED) return;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture failed");
    return;
  }

  HTTPClient http;
  String url = String(SERVER_BASE) + "/api/camera/upload";
  http.begin(url);
  http.addHeader("Content-Type", "image/jpeg");

  int code = http.POST(fb->buf, fb->len);
  if (code == 200 || code == 201) {
    Serial.printf("Snapshot uploaded (%u bytes)\n", fb->len);
  } else {
    Serial.printf("Upload failed: %d\n", code);
  }
  http.end();
  esp_camera_fb_return(fb);
}

// ─── MJPEG stream definitions ───────────────────────────────
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ─── HTTP handlers (native ESP-IDF httpd) ───────────────────

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  Serial.println("Stream client connected");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Stream: capture failed");
      res = ESP_FAIL;
      break;
    }

    size_t hlen = snprintf(part_buf, 64, STREAM_PART, fb->len);

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
    delay(30);
  }

  Serial.println("Stream client disconnected");
  return res;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
  char buf[512];
  String ip = WiFi.localIP().toString();
  snprintf(buf, sizeof(buf),
    "<h2>Mons IoT - ESP32-CAM</h2>"
    "<p>Stream: <a href='http://%s:81/stream'>MJPEG Stream</a></p>"
    "<p>Snapshot: <a href='/capture'>Single Frame</a></p>"
    "<p>IP: %s</p>",
    ip.c_str(), ip.c_str());

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, buf, strlen(buf));
}

static esp_err_t flash_on_handler(httpd_req_t *req) {
  digitalWrite(FLASH_LED_PIN, HIGH);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "Flash ON", 8);
}

static esp_err_t flash_off_handler(httpd_req_t *req) {
  digitalWrite(FLASH_LED_PIN, LOW);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "Flash OFF", 9);
}

// ─── Start HTTP servers ─────────────────────────────────────
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.send_wait_timeout = 2;  // Detect dead clients faster

  // Port 80 – regular endpoints
  config.server_port = 80;
  config.ctrl_port = 32768;

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_uri_t index_uri   = { .uri = "/",          .method = HTTP_GET, .handler = index_handler,    .user_ctx = NULL };
    httpd_uri_t capture_uri = { .uri = "/capture",    .method = HTTP_GET, .handler = capture_handler,  .user_ctx = NULL };
    httpd_uri_t flash_on    = { .uri = "/flash/on",   .method = HTTP_GET, .handler = flash_on_handler, .user_ctx = NULL };
    httpd_uri_t flash_off   = { .uri = "/flash/off",  .method = HTTP_GET, .handler = flash_off_handler,.user_ctx = NULL };

    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &flash_on);
    httpd_register_uri_handler(camera_httpd, &flash_off);
    Serial.println("HTTP server started on port 80");
  }

  // Port 81 – MJPEG stream (separate httpd so port 80 stays responsive)
  config.server_port = 81;
  config.ctrl_port = 32769;

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("Stream server started on port 81");
  }
}

// ─── Setup ──────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  if (!initCamera()) {
    Serial.println("Camera init failed, restarting...");
    delay(3000);
    ESP.restart();
  }

  connectWiFi();
  startCameraServer();
  registerWithServer();

  lastCaptureMs = millis();

  Serial.println("\n=== Mons IoT ESP32-CAM Ready ===");
  Serial.printf("  Snapshot: http://%s/capture\n", WiFi.localIP().toString().c_str());
  Serial.printf("  Stream:   http://%s:81/stream\n", WiFi.localIP().toString().c_str());
  Serial.printf("  Flash:    http://%s/flash/on | /flash/off\n", WiFi.localIP().toString().c_str());
}

// ─── Loop ───────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      registerWithServer();
      lastRegisterMs = millis();
    }
  }

  if (millis() - lastRegisterMs >= REGISTER_INTERVAL_MS) {
    lastRegisterMs = millis();
    registerWithServer();
  }

  if (millis() - lastCaptureMs >= CAPTURE_INTERVAL_MS) {
    lastCaptureMs = millis();
    uploadSnapshot();
  }

  delay(10);
}
