#pragma once
#include <Arduino.h>
#include <functional>
#include <Preferences.h>
#include <Adafruit_ST7735.h>
#include "telemetry.hpp"

struct DisplayPins { int CS, DC, RST, BL; };

struct DisplayCtor {
  DisplayPins pins;
  const char* ns;
  const char* kBright;
  const char* kLvCut;
  const char* kWifiSsid;
  const char* kWifiPass;
  std::function<float()> readSrcV;
  std::function<float()> readLoadA;
  std::function<void()>  scanAll;
  std::function<void()>  onOtaStart;
  std::function<void()>  onOtaEnd;
  std::function<void(float)> onLvCutChanged;
  std::function<void(float)> onOcpChanged;
  std::function<bool(int)>   onRfLearn;

  // NEW: LVP bypass accessors provided by main/protector
  std::function<bool()>      getLvpBypass;
  std::function<void(bool)>  setLvpBypass;
};

enum FaultBits : uint32_t {
  FLT_NONE              = 0,
  FLT_INA_LOAD_MISSING  = 1u << 0,
  FLT_INA_SRC_MISSING   = 1u << 1,
  FLT_WIFI_DISCONNECTED = 1u << 2,
  FLT_RF_MISSING        = 1u << 3,
};

class DisplayUI {
public:
  explicit DisplayUI(const DisplayCtor& c);

  void attachTFT(Adafruit_ST7735* tft, int blPin);
  void attachBrightnessSetter(std::function<void(uint8_t)> fn);
  void begin(Preferences& p);

  void setEncoderReaders(std::function<int8_t()> step,
                         std::function<bool()> ok,
                         std::function<bool()> back);
  void attachScanAll(std::function<void()> fn) { _scanAll = fn; }
  void tick(const Telemetry& t);

  void showStatus(const Telemetry& t);
  void showScanBegin(); void showScanResult(int relayIdx, const char* result); void showScanDone();
  void setFaultMask(uint32_t m);

  // OCP modal
  bool protectionAlarm(const char* title, const char* line1, const char* line2 = nullptr);

private:
  // draws
  void drawHome(bool force=false);
  void drawMenu();
  void drawMenuItem(int idx, bool selected);

  // actions & sub UIs
  void handleMenuSelect(int idx);
  void adjustBrightness();
  void adjustLvCutoff();
  void adjustOcpLimit();
  void toggleLvpBypass();          // NEW
  void wifiScanAndConnectUI();
  void wifiForget();
  void runOta();
  void showSystemInfo();

  // small helpers
  int8_t readStep(); bool okPressed(); bool backPressed();
  void   saveBrightness(uint8_t v); void saveLvCut(float v);

  // fault banner (scrolling)
  void   rebuildFaultText();        // build _faultText when mask changes
  void   drawFaultTicker(bool force=false);

  // pickers / input
  String textInput(const char* title, const String& initial, int maxLen,
                   const char* helpLine = nullptr);  // large grid keyboard
  int    listPicker(const char* title, const char** items, int count, int startIdx=0);
  int    listPickerDynamic(const char* title, std::function<const char*(int)> get, int count, int startIdx=0);

  // fields
  DisplayPins _pins;
  const char* _ns; const char* _kBright; const char* _kLvCut; const char* _kSsid; const char* _kPass;
  std::function<float()> _readSrcV, _readLoadA;
  std::function<void()>  _scanAll, _otaStart, _otaEnd;
  std::function<void(float)> _lvChanged, _ocpChanged;
  std::function<bool(int)> _rfLearn;
  std::function<bool()> _getLvpBypass;
  std::function<void(bool)> _setLvpBypass;

  Preferences* _prefs=nullptr;

  Adafruit_ST7735* _tft=nullptr;
  int _blPin = -1;
  std::function<void(uint8_t)> _setBrightness;

  std::function<int8_t()> _encStep; std::function<bool()> _encOk; std::function<bool()> _encBack;

  uint32_t _lastMs=0; bool _needRedraw=true; Telemetry _last{};
  int _menuIdx=0; int _prevMenuIdx=-1;
  uint32_t _faultMask = 0;

  // fault ticker state
  String _faultText;
  int    _faultScroll = 0;
  uint32_t _faultLastMs = 0;

  bool _inMenu = false;
  uint32_t _lastOkMs = 0;
};
