// Simple OTA updater - downloads firmware from GitHub and flashes it
// Uses Arduino Update library for simplicity and reliability
#include "Ota.hpp"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <MD5Builder.h>
#include "esp_coexist.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "prefs.hpp"

namespace Ota {

static void status(const Callbacks& cb, const char* s){ if (cb.onStatus) cb.onStatus(s); }
static void progress(const Callbacks& cb, size_t w, size_t t){ if (cb.onProgress) cb.onProgress(w,t); }

bool updateFromGithubLatest(const char* repo, const Callbacks& cb){
  // Log current partition state for diagnostics
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
  
  if (running) {
    Serial.printf("[OTA] Currently running from: %s (type=%d, subtype=%d, addr=0x%x, size=%d)\n",
      running->label, running->type, running->subtype, running->address, running->size);
  }
  
  if (update_partition) {
    Serial.printf("[OTA] Will update to: %s (type=%d, subtype=%d, addr=0x%x, size=%d)\n",
      update_partition->label, update_partition->type, update_partition->subtype, 
      update_partition->address, update_partition->size);
  } else {
    status(cb, "No OTA partition available");
    return false;
  }
  
  // Check OTA data partition state
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    Serial.printf("[OTA] Current partition state: %d\n", ota_state);
    // States: ESP_OTA_IMG_VALID=0, ESP_OTA_IMG_PENDING_VERIFY=1, 
    //         ESP_OTA_IMG_INVALID=2, ESP_OTA_IMG_ABORTED=3, ESP_OTA_IMG_NEW=4
  }
  
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
      // Look specifically for "firmware.bin" to avoid GitHub's auto-generated files
      if (name && url && strcmp(name, "firmware.bin") == 0) { 
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

  // Log download details
  Serial.printf("[OTA] Release tag: %s\n", tagName.c_str());
  Serial.printf("[OTA] Download URL: %s\n", assetUrl);

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
    char buf[64]; 
    snprintf(buf, sizeof(buf), "Download HTTP %d", code);
    status(cb, buf);
    Serial.printf("[OTA] HTTP error %d downloading firmware\n", code);
    
    // Check if we got HTML instead of binary (404 page)
    if (code == HTTP_CODE_NOT_FOUND) {
      Serial.println("[OTA] ERROR: firmware.bin not found in release!");
      Serial.println("[OTA] Please create a GitHub release with firmware.bin attached");
    }
    
    http2.end();
    return false;
  }
  
  int contentLen = http2.getSize();
  if (contentLen <= 0) {
    status(cb, "Unknown size");
    http2.end();
    return false;
  }
  
  // Sanity check: firmware should be at least 100KB and less than 2MB
  if (contentLen < 100000 || contentLen > 2000000) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Invalid size: %d bytes", contentLen);
    status(cb, buf);
    Serial.printf("[OTA] ERROR: Suspicious firmware size: %d bytes\n", contentLen);
    Serial.println("[OTA] May have downloaded HTML instead of binary");
    http2.end();
    return false;
  }
  
  char sizeBuf[32];
  snprintf(sizeBuf, sizeof(sizeBuf), "Size: %d bytes", contentLen);
  status(cb, sizeBuf);
  Serial.printf("[OTA] Firmware size: %d bytes\n", contentLen);

  // 4) Flash using Arduino Update library
  WiFiClient* stream = http2.getStreamPtr();
  
  // Verify we have a valid OTA partition
  if (!update_partition) {
    status(cb, "No OTA partition");
    Serial.println("[OTA] ERROR: No OTA partition available!");
    http2.end();
    return false;
  }
  
  Serial.printf("[OTA] Target partition: %s at 0x%x (size: %u)\n",
    update_partition->label, update_partition->address, update_partition->size);
  
  // Verify partition is large enough
  if (update_partition->size < (size_t)contentLen) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Firmware too large: %d > %u", contentLen, update_partition->size);
    status(cb, buf);
    Serial.printf("[OTA] ERROR: %s\n", buf);
    http2.end();
    return false;
  }
  
  // Clear any previous update errors
  Update.abort();
  delay(100);
  
  // Verify we downloaded a valid ESP32 firmware image
  // ESP32 images start with 0xE9 magic byte
  // Also capture first 32 bytes for detailed validation
  WiFiClient* peek_stream = http2.getStreamPtr();
  uint8_t header[32] = {0};
  int header_read = 0;
  
  // Peek at header without consuming stream
  if (peek_stream->available() >= 32) {
    // We can't peek multiple bytes, so we'll validate after first write
    uint8_t magic = peek_stream->peek();
    Serial.printf("[OTA] First byte of download: 0x%02X\n", magic);
    if (magic != 0xE9) {
      Serial.println("[OTA] ERROR: Downloaded file is not a valid ESP32 firmware!");
      Serial.println("[OTA] Expected magic byte 0xE9, this may be HTML or corrupted");
      status(cb, "Invalid firmware file");
      http2.end();
      return false;
    }
  }
  
  // Begin update with ONLY size and command type
  // Don't pass ledPin or ledOn - use default parameters
  // Don't pass label - let Update library select partition automatically
  if (!Update.begin(contentLen, U_FLASH)) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Update.begin fail: %d", Update.getError());
    status(cb, buf);
    Serial.printf("[OTA] Update.begin() failed - error: %d\n", Update.getError());
    Serial.printf("[OTA] Error string: %s\n", Update.errorString());
    http2.end();
    return false;
  }
  
  // Log which partition we're updating to
  Serial.printf("[OTA] Writing to partition: %s\n", 
    Update.isRunning() ? "OTA partition" : "unknown");
  Serial.printf("[OTA] Target partition size: %u bytes\n", Update.size());
  
  status(cb, "Writing...");
  
  // Initialize MD5 calculator to verify download integrity
  MD5Builder md5;
  md5.begin();
  
  size_t written = 0;
  uint8_t buff[1024];
  bool headerValidated = false;
  unsigned long lastDataTime = millis();
  const unsigned long DATA_TIMEOUT = 15000; // 15 second timeout for stalled downloads
  
  while (written < (size_t)contentLen) {
    size_t avail = stream->available();
    if (avail) {
      lastDataTime = millis(); // Reset timeout on data received
      int toRead = (avail > sizeof(buff)) ? sizeof(buff) : (int)avail;
      int c = stream->readBytes(buff, toRead);
      if (c <= 0) break;
      
      // Validate ESP32 image header on first chunk
      if (!headerValidated && c >= 32) {
        Serial.println("[OTA] ===== ESP32 Image Header Validation =====");
        Serial.printf("[OTA] Magic: 0x%02X (expect 0xE9)\n", buff[0]);
        Serial.printf("[OTA] Segment count: %d\n", buff[1]);
        Serial.printf("[OTA] SPI mode: 0x%02X\n", buff[2]);
        Serial.printf("[OTA] Flash config: 0x%02X\n", buff[3]);
        Serial.printf("[OTA] Entry point: 0x%02X%02X%02X%02X\n", 
          buff[7], buff[6], buff[5], buff[4]);
        Serial.printf("[OTA] First 32 bytes: ");
        for (int i = 0; i < 32; i++) {
          Serial.printf("%02X ", buff[i]);
        }
        Serial.println();
        Serial.println("[OTA] ==========================================");
        headerValidated = true;
      }
      
      // Add data to MD5 hash calculation
      md5.add(buff, c);
      
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
    Serial.printf("[OTA] Size mismatch - written: %u, expected: %d\n", (unsigned)written, contentLen);
    Update.abort();
    return false;
  }

  Serial.printf("[OTA] Download complete: %u bytes written\n", (unsigned)written);
    // CRITICAL: Verify Update library actually received the writes
  size_t updateProgress = Update.progress();
  Serial.printf("[OTA] Update.progress() reports: %u bytes\\n", (unsigned)updateProgress);
  if (updateProgress == 0) {
    status(cb, "Update state error");
    Serial.println("[OTA] ERROR: Update library reports 0 progress after write!");
    Serial.println("[OTA] This indicates Update.write() calls were not registered");
    Update.abort();
    return false;
  }
  if (updateProgress != written) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Progress mismatch: %u vs %u", (unsigned)updateProgress, (unsigned)written);
    status(cb, buf);
    Serial.printf("[OTA] WARNING: Update progress (%u) != bytes written (%u)\\n", 
      (unsigned)updateProgress, (unsigned)written);
  }
    // Calculate MD5 hash but DON'T set it in Update library
  // The Update library's own validation should handle image format checking
  md5.calculate();
  String md5Hash = md5.toString();
  Serial.printf("[OTA] Calculated MD5: %s\n", md5Hash.c_str());
  
  // DO NOT set MD5 - let Update.end() validate the ESP32 image format naturally
  // Update.setMD5(md5Hash.c_str());
  Serial.println("[OTA] Skipping MD5 check, using native ESP32 image validation");
  
  // CRITICAL: Disconnect WiFi before flash finalization to prevent interference
  // WiFi activity can cause flash write corruption during validation
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  Serial.println("[OTA] WiFi disconnected for flash finalization");
  
  // Allow flash controller time to settle before validation
  // Note: Update library buffers writes and flushes during Update.end()
  delay(500);
  
  status(cb, "Verifying...");
  
  // Verify Update object state before finalization
  if (Update.hasError()) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Write error detected: %d", Update.getError());
    status(cb, buf);
    Serial.printf("[OTA] Error detected during write phase: %d\n", Update.getError());
    Serial.printf("[OTA] Error string: %s\n", Update.errorString());
    Update.abort();
    return false;
  }

  status(cb, "Finalizing...");
  Serial.println("[OTA] Starting Update.end() validation...");
  Serial.println("[OTA] Using native ESP32 SHA256 image validation");
  
  // Call Update.end(true) without MD5 set
  // This lets the ESP32 bootloader use its built-in SHA256 validation
  // which is embedded in the firmware during build
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
    
    // Additional diagnostics
    Serial.printf("[OTA] Update progress: %u bytes\n", Update.progress());
    Serial.printf("[OTA] Update size: %u bytes\n", Update.size());
    Serial.printf("[OTA] Update remaining: %u bytes\n", Update.remaining());
    
    // Check if partition is readable after failed write
    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part) {
      Serial.printf("[OTA] Update partition: %s at 0x%x\n", update_part->label, update_part->address);
      
      // Try to verify the partition is accessible
      uint8_t test_buf[16];
      if (esp_partition_read(update_part, 0, test_buf, sizeof(test_buf)) == ESP_OK) {
        Serial.printf("[OTA] Partition readable - first bytes: %02x %02x %02x %02x\n",
          test_buf[0], test_buf[1], test_buf[2], test_buf[3]);
      } else {
        Serial.println("[OTA] ERROR: Cannot read from update partition!");
      }
    }
    
    return false;
  }

  Serial.println("[OTA] Validation passed! Update completed successfully.");
  
  // Save version tag
  if (tagName.length() > 0) {
    Preferences p; 
    p.begin(NVS_NS, false); 
    p.putString(KEY_FW_VER, tagName); 
    p.end();
    Serial.printf("[OTA] Saved version tag: %s\n", tagName.c_str());
  }

  status(cb, "OTA OK. Rebooting...");
  delay(1000);  // Give user time to see success message
  ESP.restart();
  return true;
}

} // namespace Ota
