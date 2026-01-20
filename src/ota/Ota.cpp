// File Overview: Implements the GitHub-based OTA update flow by fetching the latest
// release JSON, downloading the firmware asset, and flashing it with Update.
#include "Ota.hpp"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include "prefs.hpp"

namespace Ota {

static void status(const Callbacks& cb, const char* s){ if (cb.onStatus) cb.onStatus(s); }
static void progress(const Callbacks& cb, size_t w, size_t t){ if (cb.onProgress) cb.onProgress(w,t); }

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

  // Check partition state - refuse OTA if still pending verification
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
      if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        status(cb, "ERROR: Firmware not yet verified");
        status(cb, "Wait 10 seconds after boot");
        return false;
      }
    }
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
      if (name && url && strstr(name, ".bin")) { assetUrl = url; break; }
    }
  }
  String fallback;
  if (!assetUrl) {
    fallback = String("https://github.com/") + r + "/releases/latest/download/firmware.bin";
    assetUrl = fallback.c_str();
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

  if (!Update.begin(contentLen)) {
    status(cb, "Update.begin fail");
    http.end();
    return false;
  }

  size_t written = 0; uint8_t buff[2048];
  while (http.connected() && (remaining > 0 || contentLen == -1)) {
    size_t avail = stream->available();
    if (avail) {
      int toRead = (avail > sizeof(buff)) ? sizeof(buff) : (int)avail;
      int c = stream->readBytes(buff, toRead);
      if (c < 0) break;
      if (Update.write(buff, c) != (size_t)c) {
        status(cb, "Write error");
        Update.abort(); http.end();
        return false;
      }
      written += c;
      if (contentLen > 0) remaining -= c;
      progress(cb, written, contentLen > 0 ? (size_t)contentLen : 0);
    } else {
      yield(); // Let ESP32 handle background tasks
      delay(1);
    }
  }
  http.end();

  // Verify we wrote the expected amount
  if (contentLen > 0 && written != (size_t)contentLen) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Size mismatch: got %u, expected %d", (unsigned)written, contentLen);
    status(cb, buf);
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    status(cb, "Update.end fail");
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
