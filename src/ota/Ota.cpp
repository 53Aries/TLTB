// Simple OTA updater - downloads firmware from GitHub and flashes it
// Uses Arduino Update library for simplicity and reliability
#include "Ota.hpp"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include "esp_coexist.h"
#include "prefs.hpp"

namespace Ota {

static void status(const Callbacks& cb, const char* s){ if (cb.onStatus) cb.onStatus(s); }
static void progress(const Callbacks& cb, size_t w, size_t t){ if (cb.onProgress) cb.onProgress(w,t); }

bool updateFromGithubLatest(const char* repo, const Callbacks& cb){
  // WiFi is OFF by default - start it now using saved credentials
  // Enable WiFi/BLE coexistence (though BLE is shut down during OTA)
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
  
  status(cb, "Starting WiFi...");
  
  // Get saved WiFi credentials from NVS
  Preferences prefs;
  prefs.begin(NVS_NS, true);  // read-only
  String ssid = prefs.getString(KEY_WIFI_SSID, "");
  String pass = prefs.getString(KEY_WIFI_PASS, "");
  prefs.end();
  
  if (ssid.length() == 0) {
    status(cb, "No WiFi credentials");
    status(cb, "Configure in menu first");
    return false;
  }
  
  // Start WiFi from OFF state
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // Lower power mode
  delay(100);
  
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[OTA] Connecting to WiFi: %s\n", ssid.c_str());
  
  // Wait for connection
  int timeout = 15;
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    status(cb, "Connecting...");
    delay(1000);
    timeout--;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    status(cb, "WiFi connection failed");
    WiFi.mode(WIFI_OFF);  // Turn off WiFi on failure
    return false;
  }
  
  Serial.printf("[OTA] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  status(cb, "WiFi connected");
  delay(300);  // Allow connection to stabilize

  // 1) Get latest release info from GitHub API
  const char* r = repo && repo[0] ? repo : OTA_REPO;
  String api = String("https://api.github.com/repos/") + r + "/releases/latest";
  
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "TLTB-Updater");
  http.addHeader("Accept", "application/vnd.github+json");
  
  if (!http.begin(api.c_str())) { 
    status(cb, "URL error"); 
    return false; 
  }
  
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char buf[32]; snprintf(buf, sizeof(buf), "API HTTP %d", code);
    status(cb, buf);
    http.end();
    return false;
  }
  
  String body = http.getString();
  http.end();

  // 2) Parse JSON to find firmware URL
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, body)) { 
    status(cb, "JSON parse error"); 
    return false; 
  }

  String tagName = doc["tag_name"].as<const char*>() ? String(doc["tag_name"].as<const char*>()) : String();
  const char* assetUrl = nullptr;
  
  if (doc["assets"].is<JsonArray>()) {
    for (JsonObject a : doc["assets"].as<JsonArray>()) {
      const char* name = a["name"] | "";
      const char* url  = a["browser_download_url"] | "";
      if (name && url && strstr(name, ".bin")) { 
        assetUrl = url; 
        char nameBuf[48];
        snprintf(nameBuf, sizeof(nameBuf), "Found: %.40s", name);
        status(cb, nameBuf);
        break; 
      }
    }
  }
  
  String fallback;
  if (!assetUrl) {
    fallback = String("https://github.com/") + r + "/releases/latest/download/firmware.bin";
    assetUrl = fallback.c_str();
    status(cb, "Using fallback URL");
  }

  // 3) Download firmware
  status(cb, "Downloading...");
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // Max power for reliable download
  
  HTTPClient http2;
  http2.setTimeout(60000); // 60 second timeout
  http2.useHTTP10(true);   // HTTP/1.0 for simpler streaming
  http2.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http2.setRedirectLimit(10);
  http2.addHeader("User-Agent", "TLTB-Updater");
  http2.addHeader("Accept", "application/octet-stream");
  http2.addHeader("Connection", "keep-alive");
  
  if (!http2.begin(assetUrl)) { 
    status(cb, "Download URL error"); 
    return false; 
  }
  
  code = http2.GET();
  if (code != HTTP_CODE_OK) {
    char buf[32]; snprintf(buf, sizeof(buf), "Download HTTP %d", code);
    status(cb, buf);
    http2.end();
    return false;
  }
  
  int contentLen = http2.getSize();
  if (contentLen <= 0) {
    status(cb, "Unknown size");
    http2.end();
    return false;
  }
  
  char sizeBuf[32];
  snprintf(sizeBuf, sizeof(sizeBuf), "Size: %d bytes", contentLen);
  status(cb, sizeBuf);

  // 4) Flash using Arduino Update library
  WiFiClient* stream = http2.getStreamPtr();
  
  if (!Update.begin(contentLen)) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Update.begin fail: %d", Update.getError());
    status(cb, buf);
    http2.end();
    return false;
  }
  
  status(cb, "Writing...");
  
  size_t written = 0;
  uint8_t buff[1024];
  unsigned long lastDataTime = millis();
  const unsigned long DATA_TIMEOUT = 15000; // 15 second timeout for stalled downloads
  
  while (written < (size_t)contentLen) {
    size_t avail = stream->available();
    if (avail) {
      lastDataTime = millis(); // Reset timeout on data received
      int toRead = (avail > sizeof(buff)) ? sizeof(buff) : (int)avail;
      int c = stream->readBytes(buff, toRead);
      if (c <= 0) break;
      
      size_t w = Update.write(buff, c);
      if (w != (size_t)c) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Write fail: %d", Update.getError());
        status(cb, buf);
        Update.abort();
        http2.end();
        return false;
      }
      
      written += c;
      progress(cb, written, contentLen);
    } else {
      delay(10); // Longer delay when no data
      
      // Check for stalled download
      if (millis() - lastDataTime > DATA_TIMEOUT) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Timeout at %u/%d", (unsigned)written, contentLen);
        status(cb, buf);
        Update.abort();
        http2.end();
        return false;
      }
    }
    
    // Connection lost check
    if (!http2.connected() && !stream->available() && written < (size_t)contentLen) {
      char buf[48];
      snprintf(buf, sizeof(buf), "Lost at %u/%d", (unsigned)written, contentLen);
      status(cb, buf);
      Update.abort();
      http2.end();
      return false;
    }
  }
  
  http2.end();

  if (written != (size_t)contentLen) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Size mismatch: %u/%d", (unsigned)written, contentLen);
    status(cb, buf);
    Update.abort();
    return false;
  }

  Serial.printf("[OTA] Download complete: %u bytes written\n", (unsigned)written);
  delay(100);  // Allow flash write buffer to settle

  status(cb, "Finalizing...");
  Serial.println("[OTA] Starting Update.end() validation...");
  
  if (!Update.end(true)) {
    char buf[64];
    int err = Update.getError();
    snprintf(buf, sizeof(buf), "Validation fail: err=%d", err);
    status(cb, buf);
    Serial.printf("[OTA] Update.end() failed with error: %d\n", err);
    Serial.printf("[OTA] Bytes written: %u of %d\n", (unsigned)written, contentLen);
    if (Update.hasError()) {
      Serial.printf("[OTA] Update error string: %s\n", Update.errorString());
    }
    return false;
  }

  Serial.println("[OTA] Validation passed!");
  
  // Save version tag
  if (tagName.length() > 0) {
    Preferences p; 
    p.begin(NVS_NS, false); 
    p.putString(KEY_FW_VER, tagName); 
    p.end();
  }

  status(cb, "OTA OK. Rebooting...");
  delay(500);
  ESP.restart();
  return true;
}

} // namespace Ota
