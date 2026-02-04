#include "ble/TltbBleService.hpp"

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <esp_gap_ble_api.h>
#include <esp_log.h>
#include <mbedtls/base64.h>

#include <cmath>
#include <cstring>

namespace {
constexpr char kServiceUuid[] = "0000a11c-0000-1000-8000-00805f9b34fb";
constexpr char kStatusCharUuid[] = "0000a11d-0000-1000-8000-00805f9b34fb";
constexpr char kControlCharUuid[] = "0000a11e-0000-1000-8000-00805f9b34fb";
constexpr uint32_t kStatusIntervalMs = 1000;
constexpr size_t kStatusJsonCap = 512;
constexpr size_t kStatusBase64Cap = ((kStatusJsonCap + 2) / 3) * 4 + 4;
constexpr size_t kControlDecodeCap = 256;
const char* kBleLogTag = "TLTB-BLE";

const char* relayIdForIndex(RelayIndex idx) {
  switch (idx) {
    case R_LEFT:   return "relay-left";
    case R_RIGHT:  return "relay-right";
    case R_BRAKE:  return "relay-brake";
    case R_TAIL:   return "relay-tail";
    case R_MARKER: return "relay-marker";
    case R_AUX:    return "relay-aux";
    default:       return "";
  }
}

bool relayIndexFromId(const char* id, RelayIndex& out) {
  if (!id) return false;
  if (strcmp(id, "relay-left") == 0)   { out = R_LEFT;   return true; }
  if (strcmp(id, "relay-right") == 0)  { out = R_RIGHT;  return true; }
  if (strcmp(id, "relay-brake") == 0)  { out = R_BRAKE;  return true; }
  if (strcmp(id, "relay-tail") == 0)   { out = R_TAIL;   return true; }
  if (strcmp(id, "relay-marker") == 0) { out = R_MARKER; return true; }
  if (strcmp(id, "relay-aux") == 0)    { out = R_AUX;    return true; }
  return false;
}

void setNullableFloat(JsonObject obj, const char* key, float value) {
  if (isnan(value)) {
    obj[key] = nullptr;
  } else {
    obj[key] = value;
  }
}

bool decodeBase64(const std::string& input, uint8_t* buffer, size_t& decodedLen) {
  if (!buffer) return false;
  size_t required = 0;
  int rc = mbedtls_base64_decode(nullptr, 0, &required,
                                 reinterpret_cast<const unsigned char*>(input.data()), input.size());
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) {
    return false;
  }
  if (required > decodedLen) {
    return false;
  }
  rc = mbedtls_base64_decode(buffer, decodedLen, &decodedLen,
                             reinterpret_cast<const unsigned char*>(input.data()), input.size());
  return rc == 0;
}

}  // namespace

class TltbBleService::ServerCallbacks : public NimBLEServerCallbacks {
public:
  explicit ServerCallbacks(TltbBleService& service) : _service(service) {}

  void onConnect(NimBLEServer* server) override {
    (void)server;
    _service.handleClientConnect();
  }

  void onDisconnect(NimBLEServer* server) override {
    (void)server;
    _service.handleClientDisconnect();
    NimBLEDevice::startAdvertising();
  }

private:
  TltbBleService& _service;
};

class TltbBleService::ControlCallbacks : public NimBLECharacteristicCallbacks {
public:
  explicit ControlCallbacks(TltbBleService& service) : _service(service) {}

  void onWrite(NimBLECharacteristic* characteristic) override {
    _service.handleControlWrite(characteristic->getValue());
  }

private:
  TltbBleService& _service;
};

void TltbBleService::begin(const char* deviceName, const BleCallbacks& callbacks) {
  if (_initialized) {
    return;
  }

  _callbacks = callbacks;
  NimBLEDevice::init(deviceName && deviceName[0] ? deviceName : "TLTB Controller");
  // Max power on every BLE role; battery draw is not a constraint for this product.
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_DEFAULT);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_SCAN);
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  _server = NimBLEDevice::createServer();
  if (!_server) {
    ESP_LOGE(kBleLogTag, "Failed to create BLE server");
    return;
  }

  _server->setCallbacks(new ServerCallbacks(*this));
  NimBLEService* service = _server->createService(kServiceUuid);
  if (!service) {
    ESP_LOGE(kBleLogTag, "Failed to create BLE service");
    return;
  }

  _statusChar = service->createCharacteristic(kStatusCharUuid,
                                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* control = service->createCharacteristic(kControlCharUuid,
                                                                NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  if (!control || !_statusChar) {
    ESP_LOGE(kBleLogTag, "Failed to create BLE characteristics");
    return;
  }

  control->setCallbacks(new ControlCallbacks(*this));
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->setMinInterval(0x0020);  // 20 ms = aggressive discovery window
  advertising->setMaxInterval(0x0040);  // 40 ms ceiling keeps airtime high for range
  advertising->setMinPreferred(0x0006); // Request 7.5 ms conn interval for chatty link
  advertising->setMaxPreferred(0x0012); // Cap at 15 ms to stay responsive
  advertising->start();

  _initialized = true;
  ESP_LOGI(kBleLogTag, "BLE service ready (name=%s)", deviceName && deviceName[0] ? deviceName : "TLTB Controller");
}

void TltbBleService::publishStatus(const BleStatusContext& ctx) {
  if (!_statusChar) {
    return;
  }

  const uint32_t now = millis();
  bool due = _forceNextStatus || (_lastNotifyMs == 0) || (now - _lastNotifyMs >= kStatusIntervalMs);
  if (!due) {
    return;
  }

  _forceNextStatus = false;
  _lastNotifyMs = now;

  StaticJsonDocument<kStatusJsonCap> doc;
  JsonObject root = doc.to<JsonObject>();
  root["mode"] = (ctx.uiMode == 1) ? "RV" : "HD";
  root["activeLabel"] = ctx.activeLabel ? ctx.activeLabel : "OFF";
  root["twelveVoltEnabled"] = ctx.enableRelay;
  root["lvpLatched"] = ctx.telemetry.lvpLatched;
  root["lvpBypass"] = ctx.lvpBypass;
  root["outvLatched"] = ctx.telemetry.outvLatched;
  root["outvBypass"] = ctx.outvBypass;
  root["cooldownActive"] = ctx.telemetry.cooldownActive;
  root["cooldownSecsRemaining"] = ctx.telemetry.cooldownSecsRemaining;
  root["startupGuard"] = ctx.startupGuard;
  root["faultMask"] = ctx.faultMask;
  root["timestamp"] = ctx.timestampMs ? ctx.timestampMs : now;

  setNullableFloat(root, "loadAmps", ctx.telemetry.loadA);
  setNullableFloat(root, "srcVoltage", ctx.telemetry.srcV);
  setNullableFloat(root, "outVoltage", ctx.telemetry.outV);

  JsonObject relays = root.createNestedObject("relays");
  for (int i = 0; i < (int)R_ENABLE; ++i) {
    relays[relayIdForIndex((RelayIndex)i)] = ctx.relayStates[i];
  }

  size_t needed = measureJson(doc);
  if (needed >= kStatusJsonCap) {
    ESP_LOGW(kBleLogTag, "Status JSON truncated (%u bytes needed)", static_cast<unsigned>(needed));
    return;
  }

  char jsonBuffer[kStatusJsonCap];
  size_t jsonLen = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));
  if (jsonLen == 0 || jsonLen >= sizeof(jsonBuffer)) {
    ESP_LOGW(kBleLogTag, "Failed to serialize status JSON");
    return;
  }

  unsigned char base64Buffer[kStatusBase64Cap];
  size_t encodedLen = 0;
  int rc = mbedtls_base64_encode(base64Buffer, sizeof(base64Buffer), &encodedLen,
                                 reinterpret_cast<const unsigned char*>(jsonBuffer), jsonLen);
  if (rc != 0) {
    ESP_LOGW(kBleLogTag, "Base64 encode failed (%d)", rc);
    return;
  }

  _statusChar->setValue(base64Buffer, encodedLen);
  _statusChar->notify();
}

void TltbBleService::requestImmediateStatus() {
  _forceNextStatus = true;
}

void TltbBleService::handleControlWrite(const std::string& value) {
  if (value.empty()) {
    return;
  }

  uint8_t decoded[kControlDecodeCap];
  size_t decodedLen = sizeof(decoded);
  if (!decodeBase64(value, decoded, decodedLen)) {
    ESP_LOGW(kBleLogTag, "Failed to decode control payload");
    return;
  }

  StaticJsonDocument<kControlDecodeCap> doc;
  DeserializationError err = deserializeJson(doc, decoded, decodedLen);
  if (err) {
    ESP_LOGW(kBleLogTag, "Control JSON parse error: %s", err.c_str());
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "relay") == 0) {
    const char* relayId = doc["relayId"] | nullptr;
    bool desiredState = doc["state"].as<bool>();
    RelayIndex idx;
    if (relayIndexFromId(relayId, idx) && _callbacks.onRelayCommand) {
      _callbacks.onRelayCommand(idx, desiredState);
    }
    requestImmediateStatus();
  } else if (strcmp(type, "refresh") == 0) {
    if (_callbacks.onRefreshRequest) {
      _callbacks.onRefreshRequest();
    }
    requestImmediateStatus();
  }
}

void TltbBleService::handleClientConnect() {
  _connected = true;
}

void TltbBleService::handleClientDisconnect() {
  _connected = false;
}
