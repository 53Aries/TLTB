// File Overview: Declares the NimBLE helper that exposes status notifications and
// command handling so the mobile companion can mirror the TFT UI without
// disrupting the existing RF workflow.
#pragma once

#include <Arduino.h>
#include <functional>
#include <string>

#include "telemetry.hpp"
#include "relays.hpp"

class NimBLEServer;
class NimBLECharacteristic;

struct BleStatusContext {
  Telemetry telemetry;
  uint32_t faultMask = 0;
  bool startupGuard = false;
  bool lvpBypass = false;
  bool outvBypass = false;
  bool enableRelay = false;
  bool relayStates[R_COUNT] = {false};
  const char* activeLabel = "OFF";
  uint32_t timestampMs = 0;
  uint8_t uiMode = 0;
};

struct BleCallbacks {
  std::function<void(RelayIndex, bool)> onRelayCommand;
  std::function<void()> onRefreshRequest;
};

class TltbBleService {
public:
  void begin(const char* deviceName, const BleCallbacks& callbacks);
  void publishStatus(const BleStatusContext& ctx);
  void requestImmediateStatus();
  void stopAdvertising();
  void restartAdvertising();
  void shutdownForOta();     // Complete BLE shutdown for WiFi OTA operations
  void restartAfterOta();    // Reinitialize BLE after OTA
  bool isConnected() const { return _connected; }

private:
  class ServerCallbacks;
  class ControlCallbacks;

  void handleControlWrite(const std::string& value);
  void handleClientConnect(NimBLEServer* server);
  void handleClientDisconnect();
  void handleMtuChanged(uint16_t mtu);

  bool _initialized = false;
  bool _connected = false;
  bool _mtuNegotiated = false;
  uint16_t _negotiatedMtu = 23;  // Default BLE MTU
  bool _forceNextStatus = false;
  uint32_t _lastNotifyMs = 0;
  BleCallbacks _callbacks{};
  NimBLEServer* _server = nullptr;
  NimBLECharacteristic* _statusChar = nullptr;
  
  // Saved state for OTA restart
  String _deviceName;
  bool _wasInitialized = false;
};
