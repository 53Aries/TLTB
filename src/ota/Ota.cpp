// File Overview: Implements the GitHub-based OTA update flow by fetching the latest
// release JSON, downloading the firmware asset, and flashing it via ESP-IDF OTA API.
#include "Ota.hpp"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "prefs.hpp"

namespace Ota {

static void status(const Callbacks& cb, const char* s){ if (cb.onStatus) cb.onStatus(s); }
static void progress(const Callbacks& cb, size_t w, size_t t){ if (cb.onProgress) cb.onProgress(w,t); }

static bool checkAndRepairOtadata(const Callbacks& cb){
  // Check otadata health before OTA to prevent corruption (error 5379)
  status(cb, "Checking otadata...");
  
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) {
    status(cb, "Cannot detect running partition");
    return false;
  }
  
  // Check if we can read otadata partition state
  esp_ota_img_states_t ota_state;
  esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
  
  // If otadata is blank or uninitialized (fresh USB flash), that's OK
  // The OTA process will initialize it properly
  if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) {
    status(cb, "Otadata uninitialized (OK)");
    return true;
  }
  
  // If we can read state successfully, try to refresh by setting boot partition
  // This catches subtle corruption before it causes OTA failure
  if (err == ESP_OK) {
    err = esp_ota_set_boot_partition(running);
    if (err == ESP_OK) {
      status(cb, "Otadata OK");
      return true;
    }
  }
  
  // At this point, either get_state failed with corruption error,
  // or set_boot_partition failed - try to repair
  if (err == ESP_ERR_OTA_SELECT_INFO_INVALID || err == ESP_ERR_INVALID_ARG) {
    status(cb, "Otadata corrupt, repairing...");
    
    const esp_partition_t* otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, 
      ESP_PARTITION_SUBTYPE_DATA_OTA, 
      NULL
    );
    
    if (!otadata) {
      status(cb, "Otadata partition missing");
      return false;
    }
    
    // Erase the corrupted otadata partition
    err = esp_partition_erase_range(otadata, 0, otadata->size);
    if (err != ESP_OK) {
      char buf[48];
      snprintf(buf, sizeof(buf), "Erase fail: %d", (int)err);
      status(cb, buf);
      return false;
    }
    
    status(cb, "Otadata erased, OTA will reinit");
    // Don't try to reinitialize - let the OTA process do it
    return true;
  }
  
  // Some other unexpected error - log it but allow OTA to proceed
  char buf[64];
  snprintf(buf, sizeof(buf), "Otadata warn: %d (0x%X)", (int)err, (int)err);
  status(cb, buf);
  status(cb, "Proceeding anyway...");
  
  delay(100);
  return true;
}

static void describePartitions(const Callbacks& cb){
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next    = esp_ota_get_next_update_partition(nullptr);
  if (!running || !next) return;
  char buf[96];
  snprintf(buf, sizeof(buf), "Run %s @0x%06x Next %s @0x%06x", running->label, running->address, next->label, next->address);
  status(cb, buf);
  if (next->size) {
    char sz[64]; snprintf(sz, sizeof(sz), "Next slot %u KB", (unsigned)(next->size / 1024));
    status(cb, sz);
  }
}

static bool beginDownload(const char* url, HTTPClient& http, const Callbacks& cb){
  http.setTimeout(10000);
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(10);
  http.addHeader("User-Agent", "TLTB-Updater");
  // Hint content type for API and assets
  http.addHeader("Accept", "application/vnd.github+json, application/octet-stream");
  if (!http.begin(url)) { status(cb, "URL error"); return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char buf[48]; snprintf(buf, sizeof(buf), "HTTP %d", code);
    status(cb, buf);
    http.end();
    return false;
  }
  return true;
}

bool updateFromGithubLatest(const char* repo, const Callbacks& cb){
  if (WiFi.status() != WL_CONNECTED) { status(cb, "Wi-Fi not connected"); return false; }

  // Stabilize WiFi before OTA
  WiFi.setSleep(false);
  delay(200);

  // Check and repair otadata partition if corrupted (prevents error 5379)
  if (!checkAndRepairOtadata(cb)) {
    status(cb, "Otadata check failed");
    return false;
  }

  describePartitions(cb);

  // 1) Query latest release API
  const char* r = repo && repo[0] ? repo : OTA_REPO;
  String api = String("https://api.github.com/repos/") + r + "/releases/latest";
  HTTPClient http;
  if (!beginDownload(api.c_str(), http, cb)) return false;
  String body = http.getString();
  http.end();

  // 2) Parse JSON to find a .bin asset URL (fallback to conventional path)
  DynamicJsonDocument doc(8192);
  DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) { status(cb, "JSON parse error"); return false; }

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

  status(cb, "Downloading...");

  // Keep WiFi alive and stabilize power before heavy download
  WiFi.setSleep(false);
  delay(100); // Let power rails stabilize
  
  // Reduce WiFi TX power to prevent brown-out during download
  WiFi.setTxPower(WIFI_POWER_15dBm); // Reduced from default 20dBm

  // 3) Stream the binary to Update
  if (!beginDownload(assetUrl, http, cb)) {
    return false;
  }
  int contentLen = http.getSize();
  int remaining = contentLen;
  WiFiClient* stream = http.getStreamPtr();
  
  char sizeBuf[32];
  snprintf(sizeBuf, sizeof(sizeBuf), "Size: %d bytes", contentLen);
  status(cb, sizeBuf);

  // Use ESP-IDF OTA API directly (more reliable than Arduino Update library)
  const esp_partition_t* update_partition = esp_ota_get_next_update_partition(nullptr);
  if (!update_partition) {
    status(cb, "No update partition");
    http.end();
    return false;
  }
  
  char partBuf[64];
  snprintf(partBuf, sizeof(partBuf), "Writing to %s", update_partition->label);
  status(cb, partBuf);

  esp_ota_handle_t ota_handle;
  esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    char buf[64];
    snprintf(buf, sizeof(buf), "esp_ota_begin fail: %d", (int)err);
    status(cb, buf);
    http.end();
    return false;
  }

  size_t written = 0; uint8_t buff[2048];
  bool headerChecked = false;
  while (http.connected() && (remaining > 0 || contentLen == -1)) {
    size_t avail = stream->available();
    if (avail) {
      int toRead = (avail > sizeof(buff)) ? sizeof(buff) : (int)avail;
      int c = stream->readBytes(buff, toRead);
      if (c < 0) break;
      
      // Check first bytes - ESP32 firmware starts with 0xE9
      if (!headerChecked && c > 0) {
        headerChecked = true;
        if (buff[0] != 0xE9) {
          char hdrBuf[64];
          snprintf(hdrBuf, sizeof(hdrBuf), "Bad header: 0x%02X (want 0xE9)", buff[0]);
          status(cb, hdrBuf);
          // Show first few bytes for debug
          if (c >= 4) {
            char dbg[32];
            snprintf(dbg, sizeof(dbg), "First: %02X %02X %02X %02X", buff[0], buff[1], buff[2], buff[3]);
            status(cb, dbg);
          }
          esp_ota_abort(ota_handle);
          http.end();
          return false;
        }
      }
      if (c < 0) break;
      
      err = esp_ota_write(ota_handle, buff, c);
      if (err != ESP_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "esp_ota_write fail: %d", (int)err);
        status(cb, buf);
        esp_ota_abort(ota_handle);
        http.end();
        return false;
      }
      
      written += c;
      if (contentLen > 0) remaining -= c;
      progress(cb, written, contentLen > 0 ? (size_t)contentLen : 0);
    } else {
      yield();
      delay(1);
    }
  }
  http.end();

  // Verify size
  if (contentLen > 0 && written != (size_t)contentLen) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Size mismatch: got %u, expected %d", (unsigned)written, contentLen);
    status(cb, buf);
    esp_ota_abort(ota_handle);
    return false;
  }

  status(cb, "Finalizing...");
  
  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    char buf[64];
    snprintf(buf, sizeof(buf), "esp_ota_end fail: %d", (int)err);
    status(cb, buf);
    return false;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    char buf[64];
    snprintf(buf, sizeof(buf), "set_boot_partition fail: %d", (int)err);
    status(cb, buf);
    return false;
  }

  // Persist the version tag if we have it (so System Info can display it)
  if (tagName.length() > 0) {
    Preferences p; p.begin(NVS_NS, false); p.putString(KEY_FW_VER, tagName); p.end();
  }

  status(cb, "OTA OK. Rebooting...");
  delay(500); // Longer delay to ensure NVS write completes
  ESP.restart();
  return true; // not reached
}

} // namespace Ota
