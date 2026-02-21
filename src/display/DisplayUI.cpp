// File Overview: Implements the TFT user interface, including the status screen,
// menu workflows, Wi-Fi and OTA utilities, telemetry rendering, and fault banners.
#include <Arduino.h>
#include "DisplayUI.hpp"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <math.h>

#include "pins.hpp"
#include "prefs.hpp"
#include "relays.hpp"
#include "rf/RF.hpp"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "esp_coexist.h"
#include "ota/Ota.hpp"

// ================== NEW: force full home repaint flag ==================
// When returning from menu/settings pages that did fillScreen(), we must
// force the next Home paint to be a full-screen draw (not incremental).
static bool g_forceHomeFull = false;
// Suppress spurious OK at boot and do edge detection
// Initialize to max to suppress until begin() sets a real deadline
static uint32_t g_okIgnoreUntilMs = 0xFFFFFFFFu;
static bool g_okPrev = false;
static bool g_okInitialReleaseSeen = false;
static constexpr uint32_t kOkLongPressMs = 700;

// ---------------- Menu ----------------
static const char* const kMenuItems[] = {
  "Set LVP Cutoff",
  "LVP Bypass",
  "Set OCP Limit",
  "Set Output V Cutoff",
  "OutV Bypass",
  "Learn RF Button",
  "Clear RF Remotes",
  "Wi-Fi Connect",
  "Wi-Fi Forget",
  "OTA Update",
  "System Info"
};
static constexpr int MENU_COUNT = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
// Dev boot menu shows only Wi‑Fi and OTA entries
static const int kDevMenuMap[] = {6, 7, 8};
static constexpr int DEV_MENU_COUNT = sizeof(kDevMenuMap) / sizeof(kDevMenuMap[0]);

static const char* const OTA_URL_KEY = "ota_url";
// Preference key for HD/RV mode
// Local color for selection background (no ST77XX_DARKGREY in lib)
static const uint16_t COLOR_DARKGREY = 0x4208; // 16-bit RGB565 approx dark gray

// Relay labels (legacy helper)
// (legacy helper removed; relay labels are handled contextually where needed)

// ===================== Debounced 1P8T =====================
// Read raw mask of 1P8T inputs (LOW = selected)
static uint8_t readRotRaw() {
  uint8_t m = 0;
  if (digitalRead(PIN_ROT_P1) == LOW) m |= (1 << 0); // P1: OFF
  if (digitalRead(PIN_ROT_P2) == LOW) m |= (1 << 1); // P2: RF
  if (digitalRead(PIN_ROT_P3) == LOW) m |= (1 << 2); // P3: LEFT
  if (digitalRead(PIN_ROT_P4) == LOW) m |= (1 << 3); // P4: RIGHT
  if (digitalRead(PIN_ROT_P5) == LOW) m |= (1 << 4); // P5: BRAKE
  if (digitalRead(PIN_ROT_P6) == LOW) m |= (1 << 5); // P6: TAIL
  if (digitalRead(PIN_ROT_P7) == LOW) m |= (1 << 6); // P7: MARK
  if (digitalRead(PIN_ROT_P8) == LOW) m |= (1 << 7); // P8: AUX
  return m;
}

// Classify mask -> index [-2=N/A (no or multiple), 0..7 = P1..P8]
static int classifyMask(uint8_t m) {
  if (m == 0) return -2;                   // no position -> N/A
  if ((m & (m - 1)) != 0) return -2;       // multiple positions -> N/A
  for (int i = 0; i < 8; ++i) if (m & (1 << i)) return i; // exactly one bit set
  return -2;
}

// Debounced/hysteretic rotary label (maps to user-specified wording)
static const char* rotaryLabel() {
  // Tunables
  static const uint32_t STABLE_MS = 50;    // must persist before accepting change
  static const int SAMPLES = 3;            // quick samples per call
  static const uint32_t SAMPLE_SPACING_MS = 2;

  static int stableIdx = -2;               // accepted index (-2=N/A)
  static int pendingIdx = -3;              // candidate awaiting stability
  static uint32_t pendingSince = 0;

  // Majority vote over a few samples
  int counts[10] = {0}; // 0..7=P1..P8, 8=N/A bucket
  for (int s = 0; s < SAMPLES; ++s) {
    int idx = classifyMask(readRotRaw());
    counts[(idx >= 0) ? idx : 8]++;
    if (SAMPLES > 1 && s + 1 < SAMPLES) delay(SAMPLE_SPACING_MS);
  }
  int bestIdx = -1, bestCnt = -1;
  for (int i = 0; i < 10; ++i) { if (counts[i] > bestCnt) { bestCnt = counts[i]; bestIdx = i; } }
  int votedIdx = (bestIdx == 8) ? -2 : bestIdx;

  uint32_t now = millis();
  if (votedIdx != stableIdx) {
    if (votedIdx != pendingIdx) { pendingIdx = votedIdx; pendingSince = now; }
    if (now - pendingSince >= STABLE_MS) { stableIdx = pendingIdx; }
  } else {
    pendingIdx = stableIdx; pendingSince = now;
  }

  switch (stableIdx) {
    case -2: return "N/A";   // undefined or invalid
    case 0:  return "OFF";   // P1
    case 1:  return "RF";    // P2
    case 2:  return "LEFT";  // P3
    case 3:  return "RIGHT"; // P4
    case 4:  return "BRAKE"; // P5 - BRAKE in both modes
    case 5:  return "TAIL";  // P6
    case 6:  return (getUiMode()==1? "REV" : "MARK");  // P7 - REV in RV mode
    case 7:  return (getUiMode()==1? "Ele Brakes" : "AUX"); // P8 - Ele Brakes in RV mode
    default: return "N/A";
  }
}

// ACTIVE line mirrors rotary intent (deterministic & safe)
static String g_activeLabelOverride = ""; // Override from main.cpp (for BLE)

static void getActiveRelayStatus(String& out){
  // If main.cpp set an override (e.g., for BLE control), use it
  if (g_activeLabelOverride.length() > 0) {
    out = g_activeLabelOverride;
    return;
  }
  
  const char* rot = rotaryLabel();
  if (!strcmp(rot, "OFF")) { out = "None"; return; }
  if (!strcmp(rot, "N/A")) { out = "N/A";  return; }

  // If we're in RF mode, check what the RF module says is active
  if (!strcmp(rot, "RF")) {
    int8_t rfActive = RF::getActiveRelay();
    if (rfActive == -1) {
      out = "RF"; return;
    }
    // RF has an active relay - display it with mode-aware naming
    switch (rfActive) {
      case R_LEFT:   out = "LEFT";  return;
      case R_RIGHT:  out = "RIGHT"; return;
      case R_BRAKE:  out = "BRAKE"; return;
      case R_TAIL:   out = "TAIL";  return;
      case R_MARKER: out = (getUiMode()==1? "REV" : "MARK");  return;
      case R_AUX:    out = (getUiMode()==1? "Ele Brakes" : "AUX"); return;
      default:       out = "RF"; return;
    }
  }

  out = rot;
}

// ================================================================
// ctor / setup
// ================================================================
DisplayUI::DisplayUI(const DisplayCtor& c)
: _pins(c.pins),
  _ns(c.ns),
  _kLvCut(c.kLvCut),
  _kSsid(c.kWifiSsid),
  _kPass(c.kWifiPass),
  _readSrcV(c.readSrcV),
  _readLoadA(c.readLoadA),
  _otaStart(c.onOtaStart),
  _otaEnd(c.onOtaEnd),
  _lvChanged(c.onLvCutChanged),
  _ocpChanged(c.onOcpChanged),
  _outvChanged(c.onOutvChanged),
  _rfLearn(c.onRfLearn),
  _getOutvBypass(c.getOutvBypass),
  _setOutvBypass(c.setOutvBypass),
  _getLvpBypass(c.getLvpBypass),
  _setLvpBypass(c.setLvpBypass),
  _getStartupGuard(c.getStartupGuard),
  _bleStop(c.onBleStop),
  _bleRestart(c.onBleRestart) {}

void DisplayUI::attachTFT(Adafruit_ST7735* tft, int blPin){ _tft=tft; _blPin=blPin; }
void DisplayUI::attachBrightnessSetter(std::function<void(uint8_t)> fn){ _setBrightness=fn; }

void DisplayUI::begin(Preferences& p){
  _prefs = &p;
  if (_blPin >= 0) { pinMode(_blPin, OUTPUT); digitalWrite(_blPin, HIGH); }
  _tft->setTextWrap(false);
  _tft->fillScreen(ST77XX_BLACK);

  // Ignore OK presses briefly after boot to prevent accidental menu entry
  g_okIgnoreUntilMs = millis() + 800; // 0.8s suppress window
  g_okPrev = false;
  g_okInitialReleaseSeen = false;
  _okHolding = false;
  _okHoldLong = false;
  _okDownMs = 0;

  // Ensure 1P8T inputs are not floating: use internal pull-ups (selected = LOW)
  pinMode(PIN_ROT_P1, INPUT_PULLUP);
  pinMode(PIN_ROT_P2, INPUT_PULLUP);
  pinMode(PIN_ROT_P3, INPUT_PULLUP);
  pinMode(PIN_ROT_P4, INPUT_PULLUP);
  pinMode(PIN_ROT_P5, INPUT_PULLUP);
  pinMode(PIN_ROT_P6, INPUT_PULLUP);
  pinMode(PIN_ROT_P7, INPUT_PULLUP);
  pinMode(PIN_ROT_P8, INPUT_PULLUP);

  // Apply brightness at max (menu removed; keep at full by default)
  if (_setBrightness) _setBrightness(255);

  // Load persisted UI mode (default HD)
  _mode = _prefs->getUChar(KEY_UI_MODE, 0);

  // Splash (leave visible during boot - will be cleared by first screen draw)
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(10, 38); _tft->print("Swanger Innovations");
  _tft->setTextSize(2);
  _tft->setCursor(26, 58); _tft->print("TLTB");
  delay(900);
  // Keep splash visible - don't clear here
}

void DisplayUI::setEncoderReaders(std::function<int8_t()> s, std::function<bool()> ok, std::function<bool()> back){
  _encStep=s; _encOk=ok; _encBack=back;
}

// ================================================================
// faults
// ================================================================
void DisplayUI::setFaultMask(uint32_t m){
  if (m!=_faultMask){
    _faultMask=m;
    rebuildFaultText();
    _faultScroll=0;
    _needRedraw=true;
  }
}

void DisplayUI::setActiveLabel(const char* label) {
  if (label && label[0] != '\0') {
    g_activeLabelOverride = label;
  } else {
    g_activeLabelOverride = "";
  }
}

void DisplayUI::rebuildFaultText(){
  _faultText = "";
  auto add=[&](const char* s){
    if (_faultText.length()) _faultText += "  |  ";
    _faultText += s;
  };
  if (_faultMask & FLT_INA_LOAD_MISSING)  add("Load INA missing");
  if (_faultMask & FLT_INA_SRC_MISSING)   add("Src INA missing");
  if (_faultMask & FLT_RF_MISSING)        add("RF missing");
  if (_faultMask & FLT_RELAY_COIL)        add("Relay fault");
  if (_faultText.length()==0) _faultText = "Fault";
}

void DisplayUI::drawFaultTicker(bool force){
  const int w = 160, h = 128, barH = 18, y = h - barH;

  if (_faultMask == 0) {
    _tft->fillRect(0, y, w, barH, ST77XX_BLACK);
    return;
  }

  if (force) _faultScroll = 0;

  _tft->fillRect(0, y, w, barH, ST77XX_RED);
  _tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
  _tft->setTextSize(1);

  String msg = _faultText + "   ";
  int msgW = msg.length() * 6; // ~6 px/char
  int x0 = 4 - (_faultScroll % msgW);

  for (int rep = 0; rep < 3; ++rep) {
    int x = x0 + rep * msgW;
    if (x > w) break;
    _tft->setCursor(x, y + 2);
    _tft->print(msg);
  }
}

// ================================================================
// home / menu draw (NO-FLICKER HOME)
// ================================================================
void DisplayUI::showStatus(const Telemetry& t){
  // Check startup guard status
  bool startupGuard = _getStartupGuard ? _getStartupGuard() : false;
  
  // Layout constants for targeted clears
  const int W = 160;
  // Shrink top rows slightly and reduce spacing to fit all lines above the fault ticker
  const int GAP     = 1;
  const int hMode   = 16;  // size=2 text = 16 px
  const int hLoad   = 16;  // size=2 text = 16 px
  const int hActive = 16;  // size=2 or 1; reserve for size=2
  const int h12     = 12;
  const int hLvp       = 12;
  const int hOutv      = 12;
  const int hCooldown  = 12;
  const int yMode      = 4;
  const int yLoad      = yMode + hMode + GAP;
  const int yActive    = yLoad + hLoad + GAP;
  const int y12        = yActive + hActive + GAP;
  const int yLvp       = y12 + h12 + GAP;
  const int yOutv      = yLvp + hLvp + GAP;
  const int yCooldown  = yOutv + hOutv + GAP;
  // Footer position when fault ticker hidden; we suppress footer entirely if faults present to avoid overlap
  const int yHintNoTicker = 114;  const int hHint   = 12;

  static bool s_inited = false;
  static String s_prevActive;
  static uint32_t s_prevFaultMask = 0;
  static bool s_prevStartupGuard = false;

  // ========== NEW: force full repaint request ==========
  if (g_forceHomeFull) {
    // Reset incremental state so we repaint everything once
    s_inited = false;
    g_forceHomeFull = false;
  }

  // Precompute strings for diff
  String activeStr; getActiveRelayStatus(activeStr);

  // First-time: full draw
  if (!s_inited) {
    _tft->fillScreen(ST77XX_BLACK);
    
    // Check if startup guard is active - show prominent warning
    if (startupGuard) {
      // Show startup guard warning in red
      _tft->fillRect(0, 20, W, 80, ST77XX_RED);
      _tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
      _tft->setTextSize(2);
      _tft->setCursor(4, 30);
      _tft->print("WARNING!");
      _tft->setTextSize(1);
      _tft->setCursor(4, 55);
      _tft->print("Cycle OUTPUT to OFF");
      _tft->setCursor(4, 75);
      _tft->print("before operation");
      
      // Footer only if no fault ticker occupying bottom area
      if (_faultMask == 0) {
        _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        _tft->setCursor(4, yHintNoTicker);
        _tft->print("OK=Switch Mode");
      }
    } else {
      // Normal status display
      // Line 1: MODE (top)
      {
        _tft->fillRect(0, yMode-2, W, hMode, ST77XX_BLACK);
        _tft->setTextSize(2);
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        _tft->setCursor(4, yMode);
        _tft->print("MODE: ");
        _tft->print(_mode ? "RV" : "HD");
      }

      // Line 2: Load (color-coded by amperage)
      _tft->setTextSize(2);
      _tft->setCursor(4, yLoad);
      if (isnan(t.loadA)) {
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        _tft->print("Load:  N/A");
      } else {
        // Draw label in white
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        _tft->print("Load: ");
          // Choose value color: <15A green, 15–<20A yellow, >=20A red
        float shownA = fabsf(t.loadA);
        if (shownA > 25.5f) shownA = 25.5f; // cap display at OCP max
        // Round to nearest tenth for stable display
        shownA = roundf(shownA * 10.0f) / 10.0f;
          uint16_t valColor = ST77XX_GREEN;            // <15A
          if (shownA >= 20.0f)      valColor = ST77XX_RED;     // >=20A up to 25.5A
          else if (shownA >= 15.0f) valColor = ST77XX_YELLOW;  // 15–<20A
        _tft->setTextColor(valColor, ST77XX_BLACK);
        _tft->printf("%4.1f A", shownA);
      }

      // Line 2: Active (auto size)
      {
        String line = String("Active: ") + activeStr;
        int availPx = 160 - 4;
        int w2 = line.length() * 6 * 2;
        uint8_t sz = (w2 > availPx) ? 1 : 2;
        _tft->setTextSize(sz);
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        _tft->setCursor(4, yActive);
        _tft->print(line);
      }

      // Line 4: Batt Volt (was LVP) (colored by state: red=ACTIVE, yellow=BYPASS, green=ok) + live src voltage
      _tft->setTextSize(1);
      _tft->setCursor(4, yLvp);
      bool bypass = _getLvpBypass ? _getLvpBypass() : false;
      uint16_t lvpColor;
      if (bypass) { lvpColor = ST77XX_YELLOW; _tft->setTextColor(lvpColor, ST77XX_BLACK); _tft->print("Batt Volt: BYPASS"); }
      else if (t.lvpLatched) { lvpColor = ST77XX_RED; _tft->setTextColor(lvpColor, ST77XX_BLACK); _tft->print("Batt Volt: ACTIVE"); }
      else { lvpColor = ST77XX_GREEN; _tft->setTextColor(lvpColor, ST77XX_BLACK); _tft->print("Batt Volt: ok"); }
      // Append source voltage in same color
      _tft->print("  ");
      if (!isnan(t.srcV)) { _tft->printf("%4.1fV", t.srcV); } else { _tft->print("N/A"); }

        // Line 6: System Volt (was OUTV) status
  _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  _tft->setCursor(4, yOutv);
      bool outvBy = _getOutvBypass ? _getOutvBypass() : false;
      uint16_t outvColor;
        if (outvBy) { outvColor = ST77XX_YELLOW; _tft->setTextColor(outvColor, ST77XX_BLACK); _tft->print("System Volt: BYPASS"); }
        else if (t.outvLatched) { outvColor = ST77XX_RED; _tft->setTextColor(outvColor, ST77XX_BLACK); _tft->print("System Volt: ACTIVE"); }
        else { outvColor = ST77XX_GREEN; _tft->setTextColor(outvColor, ST77XX_BLACK); _tft->print("System Volt: ok"); }
      // Append output voltage in same color
      _tft->print("  ");
      if (!isnan(t.outV)) { _tft->printf("%4.1fV", t.outV); } else { _tft->print("N/A"); }

      // Cooldown timer line
      _tft->setCursor(4, yCooldown);
      if (t.cooldownActive) {
        _tft->setTextColor(ST77XX_RED, ST77XX_BLACK);
        _tft->printf("Cooldown: %3ds", t.cooldownSecsRemaining);
      } else if (t.cooldownSecsRemaining > 0) {
        _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        _tft->printf("Hi-Amps Time: %3ds", t.cooldownSecsRemaining);
      } else {
        _tft->setTextColor(ST77XX_GREEN, ST77XX_BLACK);
        _tft->print("Cooldown: ok");
      }

      // Draw fault ticker first (clears bottom area)
      drawFaultTicker(true);

      // Footer only if no fault ticker (draw after drawFaultTicker so it's not overwritten)
      if (_faultMask == 0) {
        _tft->setCursor(4, yHintNoTicker);
        _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        _tft->print("OK=Switch Mode");
      }
    }

    s_prevActive = activeStr;
    s_prevFaultMask = _faultMask;
    s_prevStartupGuard = startupGuard;

    _last = t;
    _needRedraw = false;
    s_inited = true;
    return;
  }

  // --- Incremental updates (no full-screen clears) ---
  
  // Startup guard changed? Force full redraw
  if (startupGuard != s_prevStartupGuard) {
    s_inited = false; // Force full redraw
    s_prevStartupGuard = startupGuard;
    showStatus(t); // Recursive call for full redraw
    return;
  }
  
  // Skip normal incremental updates if startup guard is active
  if (startupGuard) {
    _last = t;
    _needRedraw = false;
    return;
  }

  // Load A changed?
  // MODE line diff (mode or focus changed)
  static uint8_t s_prevMode = 255;
  if (s_prevMode != _mode) {
    _tft->fillRect(0, yMode-2, W, hMode, ST77XX_BLACK);
    _tft->setTextSize(2);
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    _tft->setCursor(4, yMode);
    _tft->print("MODE: "); _tft->print(_mode?"RV":"HD");
    s_prevMode = _mode;
  }

  if ((isnan(t.loadA) != isnan(_last.loadA)) ||
      (!isnan(t.loadA) && fabsf(t.loadA - _last.loadA) > 0.1f)) {
    _tft->fillRect(0, yLoad-2, W, hLoad, ST77XX_BLACK);
    _tft->setTextSize(2);
    _tft->setCursor(4, yLoad);
    if (isnan(t.loadA)) {
      _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      _tft->print("Load:  N/A");
    } else {
      // Draw label in white then value in color
      _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        _tft->print("Load: ");
      float shownA = fabsf(t.loadA);
      if (shownA > 25.5f) shownA = 25.5f; // cap display at OCP max
      // Round to nearest tenth for stable display
      shownA = roundf(shownA * 10.0f) / 10.0f;
        uint16_t valColor = ST77XX_GREEN;            // <15A
        if (shownA >= 20.0f)      valColor = ST77XX_RED;     // >=20A up to 25.5A
        else if (shownA >= 15.0f) valColor = ST77XX_YELLOW;  // 15–<20A
      _tft->setTextColor(valColor, ST77XX_BLACK);
      _tft->printf("%4.1f A", shownA);
    }
  }

  // Active label changed (or would overflow size 2)
  if (activeStr != s_prevActive) {
    _tft->fillRect(0, yActive-2, W, hActive, ST77XX_BLACK);
    String line = String("Active: ") + activeStr;
    int availPx = 160 - 4;
    int w2 = line.length() * 6 * 2;
    uint8_t sz = (w2 > availPx) ? 1 : 2;
    _tft->setTextSize(sz);
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    _tft->setCursor(4, yActive);
    _tft->print(line);
    s_prevActive = activeStr;
  }


  // InputV removed; no redraw block needed for it

  // LVP status or bypass changed?
  {
    static bool prevBypass = false;
    bool bypass = _getLvpBypass ? _getLvpBypass() : false;
    if ((t.lvpLatched != _last.lvpLatched) || (bypass != prevBypass) || (t.srcV != _last.srcV)) {
      _tft->fillRect(0, yLvp-2, W, hLvp, ST77XX_BLACK);
      _tft->setTextSize(1);
      _tft->setCursor(4, yLvp);
      uint16_t lvpColor;
      if (bypass) { lvpColor = ST77XX_YELLOW; _tft->setTextColor(lvpColor, ST77XX_BLACK); _tft->print("Batt Volt: BYPASS"); }
      else if (t.lvpLatched) { lvpColor = ST77XX_RED; _tft->setTextColor(lvpColor, ST77XX_BLACK); _tft->print("Batt Volt: ACTIVE"); }
      else { lvpColor = ST77XX_GREEN; _tft->setTextColor(lvpColor, ST77XX_BLACK); _tft->print("Batt Volt: ok"); }
      _tft->print("  ");
      if (!isnan(t.srcV)) { _tft->printf("%4.1fV", t.srcV); } else { _tft->print("N/A"); }
      prevBypass = bypass;
    }
  }

  // Output Voltage status or bypass changed?
  {
    static bool prevOutvBy = false;
    bool outvBy = _getOutvBypass ? _getOutvBypass() : false;
    if ((t.outvLatched != _last.outvLatched) || (outvBy != prevOutvBy) || (t.outV != _last.outV)) {
      _tft->fillRect(0, yOutv-2, W, hOutv, ST77XX_BLACK);
      _tft->setTextSize(1);
      _tft->setCursor(4, yOutv);
      uint16_t outvColor;
      if (outvBy) { outvColor = ST77XX_YELLOW; _tft->setTextColor(outvColor, ST77XX_BLACK); _tft->print("System Volt: BYPASS"); }
      else if (t.outvLatched) { outvColor = ST77XX_RED; _tft->setTextColor(outvColor, ST77XX_BLACK); _tft->print("System Volt: ACTIVE"); }
      else { outvColor = ST77XX_GREEN; _tft->setTextColor(outvColor, ST77XX_BLACK); _tft->print("System Volt: ok"); }
      _tft->print("  ");
      if (!isnan(t.outV)) { _tft->printf("%4.1fV", t.outV); } else { _tft->print("N/A"); }
      prevOutvBy = outvBy;
    }
  }

  // Cooldown timer changed?
  if (t.cooldownActive != _last.cooldownActive || 
      t.cooldownSecsRemaining != _last.cooldownSecsRemaining) {
    _tft->fillRect(0, yCooldown-2, W, hCooldown, ST77XX_BLACK);
    _tft->setTextSize(1);
    _tft->setCursor(4, yCooldown);
    if (t.cooldownActive) {
      _tft->setTextColor(ST77XX_RED, ST77XX_BLACK);
      _tft->printf("Cooldown: %3ds", t.cooldownSecsRemaining);
    } else if (t.cooldownSecsRemaining > 0) {
      _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
      _tft->printf("Hi-Amps Time: %3ds", t.cooldownSecsRemaining);
    } else {
      _tft->setTextColor(ST77XX_GREEN, ST77XX_BLACK);
      _tft->print("Cooldown: ok");
    }
  }

  // Fault ticker redraw if mask changed
  if (_faultMask != s_prevFaultMask) {
    drawFaultTicker(true);
    s_prevFaultMask = _faultMask;
  }

  _last = t;
  _needRedraw = false;
}

void DisplayUI::drawHome(bool force){
  if(force) _needRedraw=true;
  if(_needRedraw) showStatus(_last);
}

// Public: request a full home repaint on next draw (used after blocking modals)
void DisplayUI::requestFullHomeRepaint(){
  g_forceHomeFull = true;
  _needRedraw = true;
}

// Auto-detect battery type at startup and set LVP accordingly
void DisplayUI::detectAndSetBatteryType(){
  if (!_tft || !_readSrcV || !_lvChanged) return;
  
  // Read current battery voltage
  float srcV = _readSrcV();
  
  // Determine battery type and appropriate LVP setting
  float lvpSetting = 0.0f;
  String batteryType = "";
  String message = "";
  bool detected = false;
  
  if (isnan(srcV)) {
    // Sensor failed - already handled by fault system, skip auto-detect
    return;
  }
  
  if (srcV >= 11.0f && srcV <= 14.0f) {
    // 12V battery detected (nominal range)
    batteryType = "12V";
    lvpSetting = 10.5f;
    detected = true;
  } else if (srcV >= 17.0f && srcV <= 22.0f) {
    // 18V battery detected (nominal range)
    batteryType = "18V";
    lvpSetting = 16.5f;
    detected = true;
  } else if (srcV > 22.0f) {
    // Higher voltage - assume 18V system with high charge
    batteryType = "18V";
    lvpSetting = 16.5f;
    detected = true;
  } else if (srcV >= 9.0f && srcV < 11.0f) {
    // Low voltage, likely 12V battery that's discharged
    batteryType = "12V (Low)";
    lvpSetting = 10.5f;
    detected = true;
  } else if (srcV >= 14.0f && srcV < 17.0f) {
    // Voltage in undefined range
    detected = false;
  } else {
    // Very low voltage or other edge case
    detected = false;
  }
  
  // Display modal with result
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  
  if (detected) {
    // Successfully detected - apply setting and show confirmation
    _lvChanged(lvpSetting);
    if (_prefs) {
      _prefs->putFloat(_kLvCut, lvpSetting);
    }
    
    // Show detection result
    _tft->setTextColor(ST77XX_GREEN, ST77XX_BLACK);
    _tft->setCursor(10, 20);
    _tft->print(batteryType);
    _tft->print(" battery detected");
    
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    _tft->setCursor(10, 40);
    _tft->print("Battery: ");
    _tft->printf("%.1fV", srcV);
    
    _tft->setCursor(10, 60);
    _tft->print("Low battery protection");
    _tft->setCursor(10, 72);
    _tft->print("set for ");
    _tft->printf("%.1fV", lvpSetting);
    
  } else {
    // Could not detect - show error
    _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    _tft->setCursor(10, 20);
    _tft->print("Unable to detect");
    _tft->setCursor(10, 32);
    _tft->print("battery type");
    
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    _tft->setCursor(10, 52);
    _tft->print("Battery: ");
    _tft->printf("%.1fV", srcV);
    
    _tft->setCursor(10, 72);
    _tft->print("Manually set LVP Cutoff.");
    _tft->setCursor(10, 84);
    _tft->print("See manual.");
  }
  
  // Footer
  _tft->setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  _tft->setCursor(10, 110);
  _tft->print("Auto-clearing in 6s...");
  
  // Hold modal for 6 seconds
  delay(6000);
  
  // Clear screen and force full home repaint
  _tft->fillScreen(ST77XX_BLACK);
  requestFullHomeRepaint();
}

// New: scrolling menu (no header). Shows 8 rows and scrolls as needed.
void DisplayUI::drawMenu(){
  const int rows = 8;
  const int y0   = 8;
  const int rowH = 12;
  // Ensure menu uses size 1 text regardless of prior Home text size
  _tft->setTextSize(1);
  int total = _devMenuOnly ? DEV_MENU_COUNT : MENU_COUNT;

  static int menuTop = 0;      // first visible index
  static int prevTop = -1;     // previously drawn top
  static int prevIdx = -1;     // previously highlighted index

  if (_needRedraw) { prevTop = -1; prevIdx = -1; }

  auto drawRow = [&](int i, bool sel){
    if (i < menuTop || i >= menuTop + rows) return;
    int y = y0 + (i - menuTop) * rowH;
    uint16_t bg = sel ? ST77XX_BLUE : ST77XX_BLACK;
    _tft->fillRect(0, y-2, 160, rowH, bg);
    _tft->setTextSize(1);
    _tft->setTextColor(ST77XX_WHITE, bg);
    _tft->setCursor(6, y);
    int srcIdx = _devMenuOnly ? kDevMenuMap[i] : i;
    _tft->print(kMenuItems[srcIdx]);
  };

  // Keep selection in view
  if (_menuIdx < menuTop) menuTop = _menuIdx;
  if (_menuIdx >= menuTop + rows) menuTop = _menuIdx - rows + 1;

  // First paint or window changed → repaint the visible window
  if (prevTop != menuTop || prevIdx < 0) {
    _tft->fillScreen(ST77XX_BLACK);
    for (int i = menuTop; i < menuTop + rows && i < total; ++i) {
      drawRow(i, i == _menuIdx);
    }
    prevTop = menuTop;
    prevIdx = _menuIdx;
    return;
  }

  // Same window; just update selection highlight
  if (prevIdx != _menuIdx) {
    drawRow(prevIdx, false);
    drawRow(_menuIdx, true);
    prevIdx = _menuIdx;
  }
}

void DisplayUI::drawMenuItem(int i, bool sel){
  // Unused by the new scrolling menu; kept for compatibility.
  int y = 8 + i*12;
  uint16_t bg = sel ? ST77XX_BLUE : ST77XX_BLACK;
  uint16_t fg = ST77XX_WHITE;
  _tft->fillRect(0, y-2, 160, 12, bg);
  _tft->setTextColor(fg, bg);
  _tft->setCursor(6, y);
  int srcIdx = _devMenuOnly ? kDevMenuMap[i] : i;
  _tft->print(kMenuItems[srcIdx]);
}

// ================================================================
// input + main tick
// ================================================================
int8_t DisplayUI::readStep(){ return _encStep? _encStep():0; }
bool   DisplayUI::okPressed(){
  if (!_encOk) return false;
  bool cur = _encOk();
  uint32_t now = millis();
  // Suppress during early boot window
  if (now < g_okIgnoreUntilMs) { g_okPrev = cur; return false; }
  // Require seeing an initial release after boot before accepting presses
  if (!g_okInitialReleaseSeen) {
    if (!cur) { g_okInitialReleaseSeen = true; }
    g_okPrev = cur;
    return false;
  }
  // Edge-detect: only fire on transition from not-pressed to pressed
  bool rising = (cur && !g_okPrev);
  g_okPrev = cur;
  if (!rising) return false;
  if (now - _lastOkMs < 160) return false;  // debounce OK
  _lastOkMs = now;
  return true;
}
bool   DisplayUI::backPressed(){ return _encBack? _encBack():false; }

DisplayUI::OkPressEvent DisplayUI::pollHomeOkPress(){
  uint32_t now = millis();
  bool cur = (digitalRead(PIN_ENC_OK) == ENC_OK_ACTIVE_LEVEL);

  // Honor boot ignore window and initial release requirement
  if (now < g_okIgnoreUntilMs) {
    if (!cur) { _okHolding = false; _okHoldLong = false; }
    return OkPressEvent::None;
  }
  if (!g_okInitialReleaseSeen) {
    if (!cur) { g_okInitialReleaseSeen = true; }
    return OkPressEvent::None;
  }

  if (cur) {
    if (!_okHolding) {
      _okHolding = true;
      _okDownMs = now;
      _okHoldLong = false;
    } else if (!_okHoldLong && (now - _okDownMs) >= kOkLongPressMs) {
      _okHoldLong = true;
    }
    return OkPressEvent::None;
  }

  if (!_okHolding) return OkPressEvent::None;

  OkPressEvent evt = _okHoldLong ? OkPressEvent::Long : OkPressEvent::Short;
  _okHolding = false;
  _okHoldLong = false;

  if (evt == OkPressEvent::Short && (now - _lastOkMs) < 160) {
    return OkPressEvent::None;
  }

  _lastOkMs = now;
  return evt;
}

void DisplayUI::tick(const Telemetry& t){
  static bool wasInMenu = false;   // ========== NEW: track menu->home transition ==========

  // Detect rotary/ACTIVE changes to force a refresh
  static String s_prevActive;
  static String s_prevRotary;
  String curActive; getActiveRelayStatus(curActive);
  String curRotary = rotaryLabel();
  if (curActive != s_prevActive || curRotary != s_prevRotary) {
    _needRedraw = true;
    s_prevActive = curActive;
    s_prevRotary = curRotary;
  }

  int8_t d  = readStep();
  bool rawBack = backPressed();
  if (_ignoreMenuBack) {
    if (!rawBack) {
      _ignoreMenuBack = false; // release detected, resume normal handling
    }
    rawBack = false;           // suppress lingering BACK edge
  }
  bool back = rawBack;
  bool ok   = _inMenu ? okPressed() : false;
  OkPressEvent okEvent = _inMenu ? OkPressEvent::None : pollHomeOkPress();

  if (_inMenu) {
    int total = _devMenuOnly ? DEV_MENU_COUNT : MENU_COUNT;
    if (d)   { _menuIdx = (( _menuIdx + d ) % total + total) % total; _needRedraw = true; }
    if (ok)  {
      int srcIdx = _devMenuOnly ? kDevMenuMap[_menuIdx] : _menuIdx;
      bool stayInMenu = handleMenuSelect(srcIdx);
      if (!_devMenuOnly) {
        if (stayInMenu) {
          _ignoreMenuBack = true; // wait for release before we allow BACK to exit
        } else {
          _inMenu = false;
          _ignoreMenuBack = false;
        }
      }
      _needRedraw = true;
    }
    if (back){ if (!_devMenuOnly) { _inMenu = false; _ignoreMenuBack = false; _needRedraw = true; } }
  } else {
    if (okEvent == OkPressEvent::Long) {
      _menuIdx = 0;
      _inMenu = true;
      _ignoreMenuBack = false;
      _needRedraw = true;
    } else if (okEvent == OkPressEvent::Short) {
      toggleMode();
      _needRedraw = true;
    }
    if (back){ _needRedraw = true; }
  }

  // ========== NEW: if we just exited menu/settings, force full home redraw ==========
  if (wasInMenu && !_inMenu) {
    g_forceHomeFull = true;   // next Home draw must be full
    _needRedraw = true;       // ensure we actually draw it this frame
    _ignoreMenuBack = false;
  }
  wasInMenu = _inMenu;

  bool changedHome =
      (!_inMenu) && (
        (isnan(t.loadA) != isnan(_last.loadA)) ||
        (!isnan(t.loadA) && fabsf(t.loadA - _last.loadA) > 0.02f) ||
        (isnan(t.srcV) != isnan(_last.srcV)) ||
        (!isnan(t.srcV) && !isnan(_last.srcV) && fabsf(t.srcV - _last.srcV) > 0.05f) ||
        (isnan(t.outV) != isnan(_last.outV)) ||
        (!isnan(t.outV) && !isnan(_last.outV) && fabsf(t.outV - _last.outV) > 0.05f) ||
        (t.lvpLatched != _last.lvpLatched) ||
        (t.ocpLatched != _last.ocpLatched) ||
        (t.cooldownActive != _last.cooldownActive) ||
        (t.cooldownSecsRemaining != _last.cooldownSecsRemaining) ||
        _needRedraw
      );

  uint32_t now = millis();
  // Update every 1 second when cooldown timer is active or counting down
  uint32_t refreshInterval = (t.cooldownActive || t.cooldownSecsRemaining > 0) ? 1000 : 33;
  if (now - _lastMs >= refreshInterval) {           // 1s refresh during cooldown, otherwise ~30 Hz
    if (_inMenu) {
      if (_needRedraw || d || ok || back) drawMenu();
      _needRedraw = false;
    } else if (changedHome) {
      showStatus(t);
      _needRedraw = false;
    }
    _lastMs = now;
  }

  // scrolling ticker
  if (!_inMenu && _faultMask != 0) {
    if (millis() - _faultLastMs >= 80) {
      _faultScroll += 2;
      drawFaultTicker(false);
      _faultLastMs = millis();
    }
  }
}

// Persist and toggle mode helpers
void DisplayUI::saveMode(uint8_t m){ if (_prefs) _prefs->putUChar(KEY_UI_MODE, m); }
void DisplayUI::toggleMode(){ _mode = (_mode==0)?1:0; saveMode(_mode); }
void DisplayUI::saveOutvCut(float v){ if (_prefs) _prefs->putFloat(KEY_OUTV_CUTOFF, v); }

void DisplayUI::setDevMenuOnly(bool on){
  _devMenuOnly = on;
  if (on) { _inMenu = true; _menuIdx = 0; _ignoreMenuBack = false; _needRedraw = true; }
}

void DisplayUI::enterMenu(int startIdx){
  int total = _devMenuOnly ? DEV_MENU_COUNT : MENU_COUNT;
  if (total <= 0) {
    _menuIdx = 0;
  } else {
    if (startIdx < 0) startIdx = 0;
    if (startIdx >= total) startIdx = total - 1;
    _menuIdx = startIdx;
  }
  _inMenu = true;
  _ignoreMenuBack = false;
  _needRedraw = true;
}

// ================================================================
// actions & sub UIs
// ================================================================
bool DisplayUI::handleMenuSelect(int idx){
  bool stayInMenu = true;
  switch(idx){
    case 0: adjustLvCutoff(); break;                        // Set LVP Cutoff
    case 1: toggleLvpBypass(); break;                       // LVP Bypass
    case 2: adjustOcpLimit(); break;                        // Set OCP Limit
  case 3: adjustOutputVCutoff(); break;                   // Set Output V Cutoff
  case 4: toggleOutvBypass(); break;                      // OutV Bypass
  case 5: {                                               // Learn RF Button
      // RF Learn (simple modal)
      int sel = 0, lastSel = -1;
      _tft->fillScreen(ST77XX_BLACK);
      _tft->setTextSize(1);
      _tft->setCursor(6,8);  _tft->print("Learn RF for:");
      _tft->setCursor(6,44); _tft->print("OK=Start  BACK=Exit");

      auto drawSel = [&](int s){
        _tft->fillRect(0,20,160,16,ST77XX_BLACK);
        _tft->setCursor(6,24);
        if (s==3) {
          _tft->print("TAIL");
        } else if (s==4) {
          _tft->print(getUiMode()==1?"REV":"MARKER");
        } else if (s==5) {
          _tft->print(getUiMode()==1?"Ele Brakes":"AUX");
        } else {
          _tft->print(s==0?"LEFT":s==1?"RIGHT":s==2?"BRAKE":"?");
        }
      };

      drawSel(sel);

      // Loop until user presses BACK; allow multiple learns without exiting the menu.
      bool exitRF = false;
      while(!exitRF){
        int8_t dd = readStep();
        if (dd) {
          sel = ((sel + dd) % 6 + 6) % 6;
        }
        if (sel != lastSel) { drawSel(sel); lastSel = sel; }

        if (okPressed()) {
          // Start listening / learning
          _tft->fillRect(0,60,160,14,ST77XX_BLACK);
          _tft->setCursor(6,60); _tft->print("Listening...");
          bool ok = _rfLearn ? _rfLearn(sel) : false;

          // Show brief result and allow encoder changes while visible.
          _tft->fillRect(0,60,160,28,ST77XX_BLACK);
          _tft->setCursor(6,60); _tft->print(ok ? "Saved" : "Failed");
          _tft->setCursor(6,76); _tft->print("OK=Learn  BACK=Exit");

          uint32_t shownAt = millis();
          // brief window where encoder/back/ok are polled so user can change selection or immediately re-learn
          while (millis() - shownAt < 800) {
            int8_t dd2 = readStep();
            if (dd2) {
              sel = ((sel + dd2) % 6 + 6) % 6;
            }
            if (sel != lastSel) { drawSel(sel); lastSel = sel; }
            if (backPressed()) { exitRF = true; break; }
            if (okPressed()) { break; } // immediate re-learn of current selection
            delay(12);
          }
          // continue outer loop (either to re-learn, change selection, or exit if BACK pressed)
        }

        if (backPressed()) break;
        delay(12);
      }
    } break;
  case 6: {                                               // Clear RF Remotes
      // Clear RF Remotes (confirmation)
      _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
      _tft->setCursor(6,10); _tft->println("Clear RF Remotes");
      _tft->setCursor(6,26); _tft->println("Erase all learned");
      _tft->setCursor(6,38); _tft->println("remotes from memory?");
      _tft->setCursor(6,62); _tft->println("OK=Confirm  BACK=Cancel");
      while (true) {
        if (okPressed())   { RF::clearAll(); _tft->fillRect(6,80,148,12,ST77XX_BLACK); _tft->setCursor(6,80); _tft->print("Cleared"); delay(600); g_forceHomeFull = true; break; }
        if (backPressed()) { g_forceHomeFull = true; break; }
        delay(10);
      }
    } break;
  case 7: wifiScanAndConnectUI(); break;                  // Wi-Fi Connect
  case 8: wifiForget(); break;                            // Wi-Fi Forget
  case 9: runOta(); break;                               // OTA Update
  case 10: showSystemInfo(); break;                       // System Info
  }
  return stayInMenu;
}

// --- adjusters ---
void DisplayUI::saveLvCut(float v){ if(_prefs) _prefs->putFloat(_kLvCut, v); }

void DisplayUI::adjustLvCutoff(){
  _tft->setTextSize(1);
  float v=_prefs->getFloat(_kLvCut, 17.0f);
  _tft->fillScreen(ST77XX_BLACK); _tft->setCursor(6,10); _tft->println("Set LVP Cutoff (V)");
  while(true){
    int8_t d=readStep(); if(d){ v+=d*0.1f; if(v<9)v=9; if(v>20)v=20;
      _tft->fillRect(6,28,148,12,ST77XX_BLACK); _tft->setCursor(6,28); _tft->printf("%4.1f V", v);
    }
    if(okPressed()){ saveLvCut(v); if(_lvChanged) _lvChanged(v); break; }
    if(backPressed()) break;
    delay(8);
  }
}

void DisplayUI::adjustOcpLimit(){
  _tft->setTextSize(1);
  float cur = _prefs->getFloat(KEY_OCP, 22.0f);
  _tft->fillScreen(ST77XX_BLACK); _tft->setCursor(6,10); _tft->println("Set OCP (A)");
  while(true){
    int8_t d=readStep(); if(d){ cur+=d; if(cur<5)cur=5; if(cur>25)cur=25;
      _tft->fillRect(6,28,148,12,ST77XX_BLACK); _tft->setCursor(6,28); _tft->printf("%4.1f A", cur);
    }
    if(okPressed()){ if(_ocpChanged) _ocpChanged(cur); _prefs->putFloat(KEY_OCP, cur); break; }
    if(backPressed()) break;
    delay(8);
  }
}

// --- Output Voltage cutoff adjuster (8..16 V) ---
void DisplayUI::adjustOutputVCutoff(){
  _tft->setTextSize(1);
  float v = _prefs->getFloat(KEY_OUTV_CUTOFF, 10.0f);
  if (v < 8.0f) v = 8.0f; if (v > 16.0f) v = 16.0f;
  _tft->fillScreen(ST77XX_BLACK); _tft->setCursor(6,10); _tft->println("Set OutV Cutoff (V)");
  while(true){
    int8_t d=readStep(); if(d){ v += d*0.1f; if(v<8.0f)v=8.0f; if(v>16.0f)v=16.0f;
      _tft->fillRect(6,28,148,12,ST77XX_BLACK); _tft->setCursor(6,28); _tft->printf("%4.1f V", v);
    }
    if(okPressed()){ if(_outvChanged) _outvChanged(v); _prefs->putFloat(KEY_OUTV_CUTOFF, v); break; }
    if(backPressed()) break;
    delay(8);
  }
}

// ---------- Instant, non-blocking LVP bypass toggle ----------
void DisplayUI::toggleLvpBypass(){
  bool on = _getLvpBypass ? _getLvpBypass() : false;
  bool newState = !on;
  if (_setLvpBypass) _setLvpBypass(newState);

  // Brief confirmation splash (short toast), then return.
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,10); _tft->println("LVP Bypass");
  _tft->setCursor(6,28); _tft->print("State: ");
  _tft->print(newState ? "ON" : "OFF");
  delay(450);

  // Ensure Home will fully repaint after leaving this settings page
  g_forceHomeFull = true;
}

// Scan UI removed

// ---------- Instant, non-blocking OUTV bypass toggle ----------
void DisplayUI::toggleOutvBypass(){
  bool on = _getOutvBypass ? _getOutvBypass() : false;
  bool newState = !on;
  if (_setOutvBypass) _setOutvBypass(newState);

  // Brief confirmation splash (short toast), then return.
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,10); _tft->println("OutV Bypass");
  _tft->setCursor(6,28); _tft->print("State: ");
  _tft->print(newState ? "ON" : "OFF");
  delay(450);

  // Ensure Home will fully repaint after leaving this settings page
  g_forceHomeFull = true;
}

// ================================================================
// OCP modal
// ================================================================
bool DisplayUI::protectionAlarm(const char* title, const char* line1, const char* line2){
  _tft->fillScreen(ST77XX_RED);
  _tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
  _tft->setTextSize(2);
  _tft->setCursor(6, 6);  _tft->print(title);
  _tft->setTextSize(1);

  _tft->setCursor(6, 34); _tft->print(line1 ? line1 : "");
  if (line2){ _tft->setCursor(6, 46); _tft->print(line2); }

  _tft->fillRect(0, 108, 160, 20, ST77XX_BLACK);
  _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  _tft->setCursor(6, 112); _tft->print("OK=Dismiss");

  while (true) {
    if (okPressed())   { g_forceHomeFull = true; return true; }
    // BACK no longer cancels; ignore until OK is pressed
    delay(10);
  }
}

// ================================================================
// Wi-Fi + OTA
// ================================================================
int DisplayUI::listPicker(const char* title, const char** items, int count, int startIdx){
  return listPickerDynamic(title, [&](int i){ return items[i]; }, count, startIdx);
}

int DisplayUI::listPickerDynamic(const char* title, std::function<const char*(int)> get, int count, int startIdx){
  const int rows = 8, y0 = 18, rowH = 12;
  _tft->setTextSize(1);
  int idx = startIdx < 0 ? 0 : (startIdx >= count ? count - 1 : startIdx);
  int top = 0;

  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(4,4); _tft->print(title);

  _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  _tft->setCursor(6, y0 + rows * rowH + 2);
  _tft->print("OK=Select  BACK=Exit");

  auto drawRow = [&](int i, bool sel){
    if (i < 0 || i >= count) return;
    int y = y0 + (i - top) * rowH;
    if (y < y0 || y >= y0 + rows * rowH) return;
    uint16_t bg = sel ? ST77XX_BLUE : ST77XX_BLACK;
    _tft->fillRect(0, y - 1, 160, rowH, bg);
  _tft->setTextSize(1);
    _tft->setTextColor(ST77XX_WHITE, bg);
    _tft->setCursor(6, y);
    const char* s = get(i);
    _tft->print(s ? s : "(null)");
  };

  if (idx < top) top = idx;
  if (idx >= top + rows) top = idx - rows + 1;

  auto redrawWindow = [&](){
    _tft->fillRect(0, y0 - 1, 160, rows * rowH + 1, ST77XX_BLACK);
    for (int i = top; i < top + rows && i < count; ++i) drawRow(i, i == idx);
  };

  int prevIdx = -1, prevTop = -1;
  prevTop = top; prevIdx = idx; redrawWindow();

  while (true) {
    int8_t d = readStep();
    if (d) {
      int newIdx = ((idx + d) % count + count) % count;
      int newTop = top;
      if (newIdx < newTop) newTop = newIdx;
      if (newIdx >= newTop + rows) newTop = newIdx - rows + 1;

      if (newTop != top) { top = newTop; idx = newIdx; redrawWindow(); prevTop = top; prevIdx = idx; }
      else { drawRow(prevIdx, false); idx = newIdx; drawRow(idx, true); prevIdx = idx; }
    }
    if (okPressed())   { g_forceHomeFull = true; return idx; }
    if (backPressed()) { g_forceHomeFull = true; return -1; }
    delay(10);
  }
}

// Grid keyboard
String DisplayUI::textInput(const char* title, const String& initial, int maxLen, const char* helpLine){
  static const char* P_LO = "abcdefghijklmnopqrstuvwxyz";
  static const char* P_UP = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  static const char* P_NUM= "0123456789";
  static const char* P_SYM= "-_.:/@#?&%+!$*()[]{}=,;\\\"'<>^|~";

  const char* pages[4] = {P_LO, P_UP, P_NUM, P_SYM};
  int page = 0;

  const int SOFT_N = 7;
  const char* soft[SOFT_N] = {"abc","ABC","123","sym","spc","del","done"};

  const int COLS    = 8;
  const int CELL_W  = 19;
  const int ROW_H   = 16;
  const int X0      = 4;
  const int Y_GRID  = 38;

  const int TXT_SIZE_CHAR = 2;
  const int TXT_SIZE_SOFT = 1;

  String buf = initial;
  int sel = 0;
  int countChars = strlen(pages[page]);
  int total = SOFT_N + countChars;

  auto idxToXY = [&](int i, int& x, int& y){
    int col = i % COLS;
    int row = i / COLS;
    x = X0 + col * CELL_W;
    y = Y_GRID + row * ROW_H;
  };

  auto drawHeader = [&](){
    _tft->fillRect(0,0,160, Y_GRID-2, ST77XX_BLACK);
    _tft->setTextColor(ST77XX_CYAN, ST77XX_BLACK);
    _tft->setCursor(4, 4); _tft->print(title);
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    if (helpLine && *helpLine){ _tft->setCursor(4, 16); _tft->print(helpLine); }
    _tft->setCursor(4, 26); _tft->print(buf);
  };

  auto drawCell = [&](int i, bool selFlag){
    int x,y; idxToXY(i,x,y);
    uint16_t bg = selFlag ? ST77XX_BLUE : ST77XX_BLACK;
    _tft->fillRect(x-1, y-1, CELL_W, ROW_H, bg);
    _tft->setTextColor(ST77XX_WHITE, bg);

    if (i < SOFT_N) {
      _tft->setTextSize(TXT_SIZE_SOFT);
      _tft->setCursor(x+1, y+2);
      _tft->print(soft[i]);
    } else {
      int ci = i - SOFT_N;
      char c = pages[page][ci];
      _tft->setTextSize(TXT_SIZE_CHAR);
      _tft->setCursor(x + 3, y + 1);
      _tft->write(c);
    }
    _tft->setTextSize(1);
  };

  auto fullRedraw = [&](){
    _tft->fillScreen(ST77XX_BLACK);
    drawHeader();
    _tft->fillRect(0, Y_GRID-2, 160, 128-(Y_GRID-2), ST77XX_BLACK);
    for (int i=0;i<total;i++) drawCell(i, i==sel);
  };

  fullRedraw();
  int prevSel = sel;

  while(true){
    int8_t d = readStep();
    if (d) {
      sel = ((sel + d) % total + total) % total;
      if (sel != prevSel) { drawCell(prevSel, false); drawCell(sel, true); prevSel = sel; }
    }

    if (okPressed()){
      if (sel < SOFT_N) {
        if (sel == 0) page = 0;
        else if (sel == 1) page = 1;
        else if (sel == 2) page = 2;
        else if (sel == 3) page = 3;
        else if (sel == 4) { if ((int)buf.length() < maxLen) buf += ' '; }
        else if (sel == 5) { if (buf.length()>0) buf.remove(buf.length()-1,1); }
        else if (sel == 6) { g_forceHomeFull = true; return buf; }

        if (sel <= 3) {
          countChars = strlen(pages[page]);
          total = SOFT_N + countChars;
          sel = 0; prevSel = 0;
          fullRedraw();
        } else {
          drawHeader();
        }
      } else {
        int ci = sel - SOFT_N;
        if (ci >= 0 && ci < countChars && (int)buf.length() < maxLen) { buf += pages[page][ci]; drawHeader(); }
      }
    }

    if (backPressed()){
      if (buf.length()>0) { buf.remove(buf.length()-1,1); drawHeader(); }
      else { g_forceHomeFull = true; return buf; }
    }

    delay(10);
  }
}

// Wi-Fi flow
void DisplayUI::wifiScanAndConnectUI(){
  // Stop BLE advertising to prevent radio conflicts during WiFi operations
  if (_bleStop) _bleStop();
  
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,8);  _tft->println("Wi-Fi Connect");
  _tft->setCursor(6,22); _tft->println("Scanning...");

  // CRITICAL: Wait for BLE to fully deinitialize before starting WiFi
  // BLE shutdown includes 500ms delay, but we add extra buffer
  delay(200);
  
  // Configure WiFi/BLE coexistence to prefer WiFi during scan
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
  
  // Start WiFi from OFF state
  WiFi.mode(WIFI_STA);
  delay(100); // Allow mode change to complete
  WiFi.setSleep(true); // Required for BLE coexistence
  delay(200); // Allow WiFi radio to initialize properly

  // Use ASYNC scan to prevent blocking BLE + reduce radio contention
  int16_t n = WiFi.scanNetworks(true, false, false, 300); // async=true, 300ms/channel
  if (n == WIFI_SCAN_RUNNING) {
    // Wait for scan to complete, checking every 100ms to allow BLE to operate
    uint32_t scanStart = millis();
    while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) {
      if (millis() - scanStart > 15000) { // 15 sec timeout
        _tft->setCursor(6,38); _tft->println("Scan timeout");
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        delay(200);
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        delay(800); 
        g_forceHomeFull = true;
        if (_bleRestart) _bleRestart();
        return;
      }
      delay(100); // Let BLE process during scan
      _tft->setCursor(6,38); _tft->print(".");
    }
  }
  
  if (n <= 0) { 
    _tft->setCursor(6,38); 
    _tft->println("No networks found"); 
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    delay(200);
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    delay(800); 
    g_forceHomeFull = true;
    if (_bleRestart) _bleRestart();
    return; 
  }

  static String ss; static char sbuf[33];
  auto getter = [&](int i)->const char*{ ss = WiFi.SSID(i); ss.toCharArray(sbuf, sizeof(sbuf)); return sbuf; };
  int pick = listPickerDynamic("Choose SSID", getter, n, 0);
  if (pick < 0) { 
    WiFi.scanDelete(); 
    WiFi.mode(WIFI_OFF);
    delay(200);
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    g_forceHomeFull = true; 
    if (_bleRestart) _bleRestart(); 
    return; 
  }

  String ssid = WiFi.SSID(pick);
  bool open = (WiFi.encryptionType(pick) == WIFI_AUTH_OPEN);
  WiFi.scanDelete(); // Free scan results after extracting needed info

  String pass;
  if (!open) pass = textInput("Password", "", 63, "abc/ABC/123/sym  OK=sel  BACK=del");

  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,8); _tft->print("Connecting to "); _tft->println(ssid);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis(); int y=28;
  while (WiFi.status()!=WL_CONNECTED && millis()-start < 15000) { 
    _tft->setCursor(6,y); 
    _tft->print("."); 
    delay(100); // Reduced from 200ms for better BLE coexistence
  }

  if (WiFi.status()==WL_CONNECTED) {
    if (_prefs){ _prefs->putString(_kSsid, ssid); _prefs->putString(_kPass, pass); }
    _tft->setCursor(6,y+12); _tft->print("OK: "); _tft->println(WiFi.localIP());
    delay(700);
  } else {
    _tft->setCursor(6,y+12); _tft->println("Failed.");
    delay(700);
  }
  
  // Shut down WiFi to free antenna for BLE
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200); // Ensure WiFi fully shuts down
  
  // Restore balanced coexistence for BLE operations
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  
  g_forceHomeFull = true;
  
  // Restart BLE advertising after WiFi operations complete
  if (_bleRestart) _bleRestart();
}

void DisplayUI::wifiForget(){
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,10); _tft->println("Wi-Fi Forget...");
  if (_prefs) { _prefs->remove(_kSsid); _prefs->remove(_kPass); }
  WiFi.disconnect(true, true);
  delay(250);
  _tft->setCursor(6,28); _tft->println("Done");
  delay(500);
  g_forceHomeFull = true;
}

// OTA
void DisplayUI::runOta(){
  // Stop BLE advertising to prevent radio conflicts during WiFi/OTA operations
  if (_bleStop) _bleStop();
  
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,10); _tft->println("OTA Update");

  constexpr int STATUS_X = 6;
  constexpr int STATUS_Y = 28;
  constexpr int STATUS_WIDTH = 148;
  constexpr int STATUS_LINE_H = 12;
  constexpr int STATUS_MAX_LINES = 3;
  constexpr int PROGRESS_Y = STATUS_Y + STATUS_MAX_LINES * STATUS_LINE_H + 6;

  auto drawStatus = [&](const char* msg){
    String text = msg ? String(msg) : String("");
    _tft->fillRect(0, STATUS_Y - 2, 160, STATUS_MAX_LINES * STATUS_LINE_H + 6, ST77XX_BLACK);
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    const int charsPerLine = STATUS_WIDTH / 6; // ~6 px per char at text size 1
    for (int line = 0; line < STATUS_MAX_LINES; ++line) {
      if (!text.length()) break;
      int take = min(charsPerLine, (int)text.length());
      bool needsMore = text.length() > take;
      if (needsMore) {
        int spaceBreak = text.lastIndexOf(' ', take - 1);
        if (spaceBreak > 0) {
          take = spaceBreak + 1;
        }
      }
      String chunk = text.substring(0, take);
      chunk.trim();
      text.remove(0, take);
      text.trim();
      if (line == STATUS_MAX_LINES - 1 && (needsMore || text.length())) {
        if ((int)chunk.length() > charsPerLine - 3) chunk.remove(charsPerLine - 3);
        chunk += "...";
        text = "";
      }
      _tft->setCursor(STATUS_X, STATUS_Y + line * STATUS_LINE_H);
      _tft->print(chunk);
    }
  };

  Ota::Callbacks cb;
  cb.onStatus = [&](const char* s){
    // Wrap status text across the small TFT so error details stay visible.
    drawStatus(s);
  };
  cb.onProgress = [&](size_t w, size_t t){
    _tft->fillRect(STATUS_X, PROGRESS_Y, STATUS_WIDTH, 10, ST77XX_BLACK);
    _tft->setCursor(STATUS_X, PROGRESS_Y);
    if (t) _tft->printf("%u/%u", (unsigned)w, (unsigned)t);
    else   _tft->printf("%u", (unsigned)w);
  };

  // Use default repo from build flag OTA_REPO; pass nullptr to use fallback
  bool ok = Ota::updateFromGithubLatest(nullptr, cb);
  
  if (!ok) {
    _tft->setCursor(6,92); _tft->println("OTA failed");
    
    // Shut down WiFi to free antenna for BLE
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    
    // Restore balanced coexistence for BLE operations
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    
    delay(900);
    g_forceHomeFull = true;
    // Restart BLE after failed OTA
    if (_bleRestart) _bleRestart();
  }
  // Note: If OTA succeeds, device will reboot - no need to restart BLE or shut down WiFi
}

// ================================================================
// Info page
// ================================================================
void DisplayUI::showSystemInfo(){
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(4, 6);  _tft->setTextColor(ST77XX_CYAN); _tft->println("System Info & Faults");
  _tft->setTextColor(ST77XX_WHITE);

  int y=22;
  auto line=[&](const char* k, const char* v){ _tft->setCursor(4,y); _tft->print(k); _tft->print(": "); _tft->println(v); y+=12; };

  // Firmware version string: pull from NVS (written by OTA on success)
  String ver = _prefs ? _prefs->getString(KEY_FW_VER, "") : String("");
  if (ver.length() == 0) ver = "unknown";
  line("Firmware", ver.c_str());

  String wifi = (WiFi.status()==WL_CONNECTED) ? String("OK ") + WiFi.localIP().toString() : "not linked";
  line("Wi-Fi", wifi.c_str());
  bool bypass = _getLvpBypass ? _getLvpBypass() : false;
  line("LVP bypass", bypass ? "ON" : "OFF");

  if (_faultMask==0) line("Faults", "None");
  else {
    if (_faultMask & FLT_INA_LOAD_MISSING)  line("Load INA226", "MISSING (0x40)");
    if (_faultMask & FLT_INA_SRC_MISSING)   line("Src INA226",  "MISSING (0x41)");
    if (_faultMask & FLT_WIFI_DISCONNECTED) line("Wi-Fi",       "Disconnected");
    if (_faultMask & FLT_RF_MISSING)        line("RF",          "Module not detected");
  }

  _tft->setTextColor(ST77XX_YELLOW);
  _tft->setCursor(4, y+4);
  _tft->println("BACK=Exit");
  while(!backPressed()){ delay(10); }
  g_forceHomeFull = true;
}
