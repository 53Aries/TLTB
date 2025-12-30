#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "esp_wifi.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

#include "pins.hpp"
#include "prefs.hpp"

static Adafruit_ST7735* tft = nullptr;
static Preferences prefs;
static WebServer server(80);

static bool g_statusDirty = true;
static bool g_staConnected = false;
static IPAddress g_staIp;
static String g_staSsid;
static IPAddress g_apIp;
static String g_apSsid;
static String g_lastAction = "Recovery ready";
static String g_uploadResult = "<p>No upload yet.</p>";
static uint32_t g_lastDrawMs = 0;

static bool g_okHolding = false;
static uint32_t g_okDownMs = 0;

static void setBacklight(uint8_t level) { ledcWrite(0, level); }

static void startDisplay() {
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, LOW);
  pinMode(PIN_TFT_CS, OUTPUT);
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_RST, OUTPUT);
  pinMode(PIN_FSPI_SCK, OUTPUT);
  pinMode(PIN_FSPI_MOSI, OUTPUT);
  pinMode(PIN_FSPI_MISO, INPUT);

  SPI.begin(PIN_FSPI_SCK, PIN_FSPI_MISO, PIN_FSPI_MOSI, PIN_TFT_CS);
  digitalWrite(PIN_TFT_RST, HIGH); delay(50);
  digitalWrite(PIN_TFT_RST, LOW);  delay(120);
  digitalWrite(PIN_TFT_RST, HIGH); delay(150);

  tft = new Adafruit_ST7735(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
  tft->setSPISpeed(8000000UL);
  tft->initR(INITR_BLACKTAB);
  tft->setRotation(1);
  tft->fillScreen(ST77XX_BLACK);

  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_TFT_BL, 0);
  setBacklight(255);
}

static void forceRelaySafeState() {
  constexpr size_t relayCount = sizeof(RELAY_PIN) / sizeof(RELAY_PIN[0]);
  for (size_t i = 0; i < relayCount; ++i) {
    pinMode(RELAY_PIN[i], INPUT);
  }
}

static void startAccessPoint() {
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "TLTB-Recovery-%02X%02X", mac[4], mac[5]);
  g_apSsid = ssid;
  WiFi.softAP(g_apSsid.c_str());
  g_apIp = WiFi.softAPIP();
  g_statusDirty = true;
}

static void connectStation() {
  String ssid = prefs.getString(KEY_WIFI_SSID, "");
  String pass = prefs.getString(KEY_WIFI_PASS, "");
  if (ssid.isEmpty()) {
    WiFi.disconnect(true);
    g_lastAction = "No STA creds saved";
    g_statusDirty = true;
    return;
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  g_lastAction = "Connecting to " + ssid;
  g_statusDirty = true;
}

static void setupWifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  startAccessPoint();
  connectStation();
}

static void drawStatus() {
  uint32_t now = millis();
  if (!g_statusDirty && (now - g_lastDrawMs) < 500) return;
  if (!tft) return;
  g_statusDirty = false;
  g_lastDrawMs = now;

  tft->fillScreen(ST77XX_BLACK);
  tft->setTextWrap(false);
  tft->setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft->setTextSize(2);
  tft->setCursor(6, 6);  tft->print("Recovery");
  tft->setTextSize(1);
  tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);

  int y = 30;
  auto line = [&](const String& text){
    tft->fillRect(0, y-2, 160, 12, ST77XX_BLACK);
    tft->setCursor(4, y);
    tft->print(text);
    y += 12;
  };

  String staLine = "STA: ";
  if (g_staConnected) {
    staLine += g_staSsid;
  } else {
    staLine += "not linked";
  }
  line(staLine);

  String ipLine = "STA IP: ";
  ipLine += g_staConnected ? g_staIp.toString() : "--";
  line(ipLine);

  line(String("AP: ") + g_apSsid);
  line(String("AP IP: ") + g_apIp.toString());

  String visit = "Open http://";
  visit += g_staConnected ? g_staIp.toString() : g_apIp.toString();
  line(visit);
  line("Upload + OTA from web UI");
  line("Hold OK to boot main");

  tft->fillRect(0, 108, 160, 20, ST77XX_BLACK);
  tft->setCursor(4, 110);
  tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  tft->print(g_lastAction);
}

static void updateStaState() {
  wl_status_t s = WiFi.status();
  bool connected = (s == WL_CONNECTED);
  if (connected != g_staConnected) {
    g_staConnected = connected;
    g_staSsid = connected ? WiFi.SSID() : "";
    g_staIp = connected ? WiFi.localIP() : IPAddress();
    g_lastAction = connected ? (String("STA linked: ") + g_staSsid) : String("STA disconnected");
    g_statusDirty = true;
  }
}

static const esp_partition_t* findOtaSlot(int slotIdx) {
  if (slotIdx < 0 || slotIdx > 1) return nullptr;
  esp_partition_subtype_t subtype = static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slotIdx);
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
}

static void rebootToMainSlot(int slotOverride, const char* reason) {
  int slot = slotOverride;
  if (slot < 0) {
    uint8_t stored = prefs.getUChar(KEY_LAST_GOOD_OTA, 0);
    if (stored > 1) stored = 0;
    slot = stored;
  }
  const esp_partition_t* part = findOtaSlot(slot);
  if (!part) {
    g_lastAction = "Main slot missing";
    g_statusDirty = true;
    return;
  }
  esp_err_t err = esp_ota_set_boot_partition(part);
  if (err != ESP_OK) {
    g_lastAction = String("Boot sel fail: ") + esp_err_to_name(err);
    g_statusDirty = true;
    return;
  }
  if (tft) {
    tft->fillScreen(ST77XX_BLACK);
    tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft->setTextSize(2);
    tft->setCursor(12, 50);
    tft->print("Booting");
    tft->setCursor(12, 70);
    tft->print("main...");
    tft->setTextSize(1);
    tft->setCursor(12, 100);
    tft->printf("slot OTA%d", slot);
    if (reason) {
      tft->setCursor(12, 112);
      tft->print(reason);
    }
  }
  delay(250);
  esp_restart();
}

static bool okLongPress() {
  bool pressed = (digitalRead(PIN_ENC_OK) == ENC_OK_ACTIVE_LEVEL);
  uint32_t now = millis();
  const uint32_t holdMs = 1500;
  if (pressed) {
    if (!g_okHolding) {
      g_okHolding = true;
      g_okDownMs = now;
    } else if (now - g_okDownMs >= holdMs) {
      g_okHolding = false;
      return true;
    }
  } else {
    g_okHolding = false;
  }
  return false;
}

static void handleRoot();
static void handleWifiSave();
static void handleWifiForget();
static void handleBootMain();
static void handleOtaDownload();
static void handleOtaUpload();

static bool performHttpOta(const String& url, String& note) {
  HTTPClient http;
  if (!http.begin(url)) {
    note = "Invalid URL";
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    note = String("HTTP ") + code;
    http.end();
    return false;
  }
  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
  if (!update) {
    note = "No OTA partition";
    http.end();
    return false;
  }
  if (!Update.begin(update->size)) {
    note = String("Update begin failed: ") + Update.errorString();
    http.end();
    return false;
  }
  size_t written = Update.writeStream(*stream);
  if (len > 0 && written != (size_t)len) {
    note = "Incomplete download";
    Update.abort();
    http.end();
    return false;
  }
  bool ok = Update.end(true) && Update.isFinished();
  if (!ok) {
    note = String("OTA error: ") + Update.errorString();
    Update.abort();
  } else {
    note = "OTA download complete";
  }
  http.end();
  return ok;
}

static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length());
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

static void handleRoot() {
  String url = g_staConnected ? g_staIp.toString() : g_apIp.toString();
  String last = htmlEscape(g_lastAction);
  String html;
  html.reserve(4096);
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'><title>TLTB Recovery</title>";
  html += "<style>body{font-family:Inter,Segoe UI,Arial;background:#0b1521;color:#f5f6f8;margin:0;padding:32px;}";
  html += "section{background:#111c2c;border:1px solid #1f2b3e;border-radius:12px;padding:20px;margin-bottom:20px;}";
  html += "label{display:block;margin-bottom:6px;font-weight:600;}input,select{width:100%;padding:8px;border-radius:6px;border:1px solid #24344e;background:#0b1521;color:#f5f6f8;margin-bottom:12px;}";
  html += "button{background:#3fb68b;border:none;padding:10px 18px;border-radius:6px;color:#04121f;font-weight:600;cursor:pointer;}h1{margin-top:0;}a{color:#68c3ff;}code{background:#08111d;padding:2px 4px;border-radius:4px;}</style></head><body>";
  html += "<h1>TLTB Recovery Console</h1>";
  html += "<p>Device AP: <strong>" + htmlEscape(g_apSsid) + "</strong><br>STA status: " + (g_staConnected ? "Connected to <strong>" + htmlEscape(g_staSsid) + "</strong>" : String("Not connected")) + "<br>Web UI address: <code>http://" + url + "</code></p>";

  html += "<section><h2>Wi-Fi Setup</h2><form method='post' action='/wifi'><label>SSID</label><input name='ssid' maxlength='32' required><label>Password</label><input name='pass' maxlength='64' type='password'><button type='submit'>Save &amp; Connect</button></form>";
  html += "<form method='post' action='/wifi/forget'><button type='submit'>Forget Wi-Fi</button></form></section>";

  html += "<section><h2>OTA Update</h2><form method='post' action='/ota/upload' enctype='multipart/form-data'><label>Firmware .bin</label><input type='file' name='firmware' accept='.bin' required><button type='submit'>Upload Firmware</button></form>";
  html += g_uploadResult;
  html += "<form method='post' action='/ota/url'><label>Direct Download URL</label><input name='url' placeholder='https://example.com/firmware.bin' required><button type='submit'>Fetch &amp; Install</button></form></section>";

  html += "<section><h2>Boot Main Firmware</h2><form method='post' action='/boot-main'><label>Target Slot</label><select name='slot'>";
  html += "<option value='auto'>Last known good</option>";
  html += "<option value='ota0'>OTA0</option><option value='ota1'>OTA1</option></select><button type='submit'>Reboot to Main</button></form>";
  html += "<p>Status: " + last + "</p></section>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

static void handleWifiSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim(); pass.trim();
  prefs.putString(KEY_WIFI_SSID, ssid);
  prefs.putString(KEY_WIFI_PASS, pass);
  g_lastAction = "Saved Wi-Fi: " + ssid;
  g_statusDirty = true;
  connectStation();
  server.send(200, "text/html", "<p>Credentials saved. Connecting...</p><a href='/'>Back</a>");
}

static void handleWifiForget() {
  prefs.remove(KEY_WIFI_SSID);
  prefs.remove(KEY_WIFI_PASS);
  WiFi.disconnect(true, true);
  g_lastAction = "Wi-Fi cleared";
  g_statusDirty = true;
  server.send(200, "text/html", "<p>Wi-Fi credentials cleared.</p><a href='/'>Back</a>");
}

static void handleBootMain() {
  String slot = server.arg("slot");
  int idx = -1;
  if (slot == "ota0") idx = 0;
  else if (slot == "ota1") idx = 1;
  server.send(200, "text/html", "<p>Rebooting to main firmware...</p>");
  delay(100);
  rebootToMainSlot(idx, "Web request");
}

static void handleOtaDownload() {
  String url = server.arg("url");
  if (url.isEmpty()) {
    server.send(400, "text/html", "<p>URL required.</p>");
    return;
  }
  String note;
  bool ok = performHttpOta(url, note);
  g_lastAction = ok ? "OTA ready – reboot main" : note;
  g_statusDirty = true;
  String body = String("<p>") + note + (ok ? "</p><p>Use the button or form to boot main.</p>" : "</p>");
  server.send(ok ? 200 : 500, "text/html", body + "<a href='/'>Back</a>");
}

static void handleOtaUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    g_lastAction = "Receiving " + upload.filename;
    g_statusDirty = true;
    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (!update || !Update.begin(update->size)) {
      g_uploadResult = "<p>OTA init failed.</p>";
      return;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.isRunning()) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        g_uploadResult = "<p>Write error.</p>";
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true) && Update.isFinished()) {
      g_uploadResult = "<p>Upload complete. Reboot to main when ready.</p>";
      g_lastAction = "OTA ready – reboot main";
    } else {
      g_uploadResult = String("<p>OTA failed: ") + Update.errorString() + "</p>";
      Update.abort();
    }
    g_statusDirty = true;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    g_uploadResult = "<p>Upload aborted.</p>";
    g_statusDirty = true;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ENC_OK, INPUT_PULLUP);
  pinMode(PIN_ENC_BACK, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);

  startDisplay();
  forceRelaySafeState();

  prefs.begin(NVS_NS, false);

  setupWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.on("/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/boot-main", HTTP_POST, handleBootMain);
  server.on("/ota/url", HTTP_POST, handleOtaDownload);
  server.on("/ota/upload", HTTP_POST,
    [](){ server.send(200, "text/html", g_uploadResult + "<a href='/'>Back</a>"); },
    handleOtaUpload);
  server.onNotFound([](){ server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); });
  server.begin();

  g_lastAction = "Recovery UI online";
  g_statusDirty = true;
}

void loop() {
  server.handleClient();
  updateStaState();
  drawStatus();
  if (okLongPress()) {
    rebootToMainSlot(-1, "OK held");
  }
  delay(5);
}