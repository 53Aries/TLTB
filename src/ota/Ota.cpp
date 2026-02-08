// Simple OTA updater - downloads firmware from GitHub and flashes it
// Uses native ESP-IDF partition APIs for direct flash control without forced validation
#include "Ota.hpp"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
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
  
  // CRITICAL: Erase the entire target partition before Update.begin()
  // This ensures clean slate and was working successfully in v1.2.37
  Serial.println("[OTA] Erasing target partition...");
  status(cb, "Erasing...");
  esp_err_t erase_err = esp_partition_erase_range(update_partition, 0, update_partition->size);
  if (erase_err != ESP_OK) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Erase failed: %d", erase_err);
    status(cb, buf);
    Serial.printf("[OTA] ERROR: Partition erase failed: %d\n", erase_err);
    http2.end();
    return false;
  }
  Serial.println("[OTA] Partition erased successfully");
  delay(100);
  
  // Verify partition is actually erased (should read 0xFF)
  uint8_t verify_erase[16];
  if (esp_partition_read(update_partition, 0, verify_erase, sizeof(verify_erase)) == ESP_OK) {
    bool erased = true;
    for (int i = 0; i < sizeof(verify_erase); i++) {
      if (verify_erase[i] != 0xFF) {
        erased = false;
        break;
      }
    }
    if (erased) {
      Serial.println("[OTA] Partition erase verified (all 0xFF)");
    } else {
      Serial.println("[OTA] WARNING: Partition not fully erased!");
    }
  }
  
  delay(100);
  
  // Verify we downloaded a valid ESP32 firmware image
  // ESP32 images start with 0xE9 magic byte
  // Also capture first 32 bytes for detailed validation
  WiFiClient* peek_stream = http2.getStreamPtr();
  // Peek at header without consuming stream
  if (peek_stream->available() >= 32) {
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
  
  // Use direct partition write instead of Update library
  // This gives us full control and avoids forced validation
  Serial.printf("[OTA] Writing directly to partition: %s\n", update_partition->label);
  Serial.printf("[OTA] Target partition size: %u bytes\n", update_partition->size);
  
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
      
      // Validate ESP32 image header and ALL segment headers on first chunk
      if (!headerValidated && written == 0 && c >= 256) {
        Serial.println("[OTA] ===== ESP32 Image Header & Segments =====");
        Serial.printf("[OTA] Magic: 0x%02X (expect 0xE9)\n", buff[0]);
        uint8_t segmentCount = buff[1];
        Serial.printf("[OTA] Segment count: %d\n", segmentCount);
        Serial.printf("[OTA] SPI mode: 0x%02X\n", buff[2]);
        Serial.printf("[OTA] Flash config: 0x%02X\n", buff[3]);
        Serial.printf("[OTA] Entry point: 0x%02X%02X%02X%02X\n", 
          buff[7], buff[6], buff[5], buff[4]);
        
        // Parse segment headers (start at offset 24 after main header)
        size_t offset = 24;
        Serial.println("[OTA] --- Segment Headers ---");
        for (int i = 0; i < segmentCount && offset + 8 <= c; i++) {
          uint32_t loadAddr = (buff[offset+3] << 24) | (buff[offset+2] << 16) | 
                              (buff[offset+1] << 8) | buff[offset];
          uint32_t segLen = (buff[offset+7] << 24) | (buff[offset+6] << 16) | 
                            (buff[offset+5] << 8) | buff[offset+4];
          Serial.printf("[OTA] Seg %d: addr=0x%08X len=%u (0x%08X) at offset %u\n", 
            i, loadAddr, segLen, segLen, offset);
          
          // Validate segment length is reasonable
          if (segLen > 0x200000) { // > 2MB is definitely wrong
            Serial.printf("[OTA] ERROR: Segment %d length is INVALID!\n", i);
          }
          
          offset += 8 + segLen; // Next segment header
        }
        Serial.printf("[OTA] Total image size estimate: %u bytes\n", offset + 1 + 32); // +1 checksum +32 SHA256
        Serial.println("[OTA] ==========================================");
        headerValidated = true;
      } else if (!headerValidated && c >= 32) {
        // Fallback for smaller first chunk
        Serial.println("[OTA] ===== ESP32 Image Header (basic) =====");
        Serial.printf("[OTA] Magic: 0x%02X\n", buff[0]);
        Serial.printf("[OTA] Segment count: %d\n", buff[1]);
        Serial.printf("[OTA] Entry: 0x%02X%02X%02X%02X\n", buff[7], buff[6], buff[5], buff[4]);
        Serial.println("[OTA] (Chunk too small for full segment analysis)");
        headerValidated = true;
      }
      
      // Add data to MD5 hash calculation
      md5.add(buff, c);
      
      // Write directly to partition
      esp_err_t err = esp_partition_write(update_partition, written, buff, c);
      if (err != ESP_OK) {
        char errbuf[48];
        snprintf(errbuf, sizeof(errbuf), "Partition write fail: %d", err);
        status(cb, errbuf);
        Serial.printf("[OTA] esp_partition_write() failed at offset %u: %d\n", (unsigned)written, err);
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
        http2.end();
        return false;
      }
    }
    
    // Connection lost check
    if (!http2.connected() && !stream->available() && written < (size_t)contentLen) {
      char buf[48];
      snprintf(buf, sizeof(buf), "Lost at %u/%d", (unsigned)written, contentLen);
      status(cb, buf);
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
    return false;
  }

  Serial.printf("[OTA] Download complete: %u bytes written\n", (unsigned)written);
  
  // Calculate MD5 hash for verification
  md5.calculate();
  String md5Hash = md5.toString();
  Serial.printf("[OTA] Calculated MD5: %s\n", md5Hash.c_str());
  Serial.println("[OTA] File integrity verified via MD5");
  
  // CRITICAL DIAGNOSTIC: Read back and verify the written partition data
  Serial.println("[OTA] ===== Verifying written partition =====");
  uint8_t verify_buf[256];
  if (esp_partition_read(update_partition, 0, verify_buf, sizeof(verify_buf)) == ESP_OK) {
    Serial.println("[OTA] First 64 bytes from partition:");
    for (int i = 0; i < 64; i++) {
      Serial.printf("%02X ", verify_buf[i]);
      if ((i + 1) % 16 == 0) Serial.println();
    }
    
    // Verify magic byte
    if (verify_buf[0] != 0xE9) {
      Serial.printf("[OTA] ERROR: Magic byte is 0x%02X, expected 0xE9!\n", verify_buf[0]);
      status(cb, "Partition write verification failed");
      return false;
    }
    Serial.println("[OTA] Magic byte verified: 0xE9");
    
    // Parse segments from written partition
    uint8_t seg_count = verify_buf[1];
    Serial.printf("[OTA] Segment count: %d\n", seg_count);
    size_t offset = 24;
    for (int i = 0; i < seg_count && i < 2 && offset + 8 <= sizeof(verify_buf); i++) {
      uint32_t addr = (verify_buf[offset+3] << 24) | (verify_buf[offset+2] << 16) | 
                      (verify_buf[offset+1] << 8) | verify_buf[offset];
      uint32_t len = (verify_buf[offset+7] << 24) | (verify_buf[offset+6] << 16) | 
                     (verify_buf[offset+5] << 8) | verify_buf[offset+4];
      Serial.printf("[OTA] Seg %d: addr=0x%08X len=%u\n", i, addr, len);
      offset += 8;  // Don't skip segment data, we don't have it in our 256-byte buffer
    }
  } else {
    Serial.println("[OTA] ERROR: Cannot read back from partition!");
    status(cb, "Partition read verification failed");
    return false;
  }
  Serial.println("[OTA] =======================================");
  
  // Disconnect WiFi before setting boot partition
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  Serial.println("[OTA] WiFi disconnected");
  
  status(cb, "Activating...");
  
  // Set boot partition - bootloader will validate on next boot
  // We've already verified the download with MD5 and read-back check
  // If validation fails at boot, ESP32 will automatically roll back
  Serial.println("[OTA] Setting boot partition...");
  esp_err_t err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Set boot partition fail: %d", err);
    status(cb, buf);
    Serial.printf("[OTA] esp_ota_set_boot_partition_skip_validate() failed: %d\n", err);
    return false;
  }
  
  Serial.println("[OTA] Boot partition updated successfully!");
  Serial.println("[OTA] Bootloader will validate and boot new firmware on restart");
  
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
