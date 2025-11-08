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
#include "ota/Ota.hpp"

// ================== NEW: force full home repaint flag ==================
// When returning from menu/settings pages that did fillScreen(), we must
// force the next Home paint to be a full-screen draw (not incremental).
static bool g_forceHomeFull = false;

// ---------------- Menu ----------------
static const char* const kMenuItems[] = {
  "Set LVP Cutoff",
  "LVP Bypass",
  "Set OCP Limit",
  "Set Output V Cutoff",
  "OutV Bypass",
  "12V System",
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
    case 4:  return "BRAKE"; // P5
    case 5:  return (getUiMode()==1? "REV" : "TAIL");  // P6
    case 6:  return "MARK";  // P7
    case 7:  return (getUiMode()==1? "EleBrake" : "AUX"); // P8
    default: return "N/A";
  }
}

// ACTIVE line mirrors rotary intent (deterministic & safe)
static void getActiveRelayStatus(String& out){
  const char* rot = rotaryLabel();
  if (!strcmp(rot, "OFF")) { out = "None"; return; }
  if (!strcmp(rot, "N/A")) { out = "N/A";  return; }

  // If we're in RF mode, reflect whichever single relay is currently ON
  if (!strcmp(rot, "RF")) {
    int onIdx = -1; int onCount = 0;
    for (int i = 0; i < (int)R_COUNT; ++i) {
      if (g_relay_on[i]) { onIdx = i; onCount++; }
    }
    if (onCount == 1) {
      switch (onIdx) {
        case R_LEFT:   out = "LEFT";  return;
        case R_RIGHT:  out = "RIGHT"; return;
        case R_BRAKE:  out = "BRAKE"; return;
        case R_TAIL:   out = (getUiMode()==1? "REV" : "TAIL");  return;
        case R_MARKER: out = "MARK";  return;
        case R_AUX:    out = (getUiMode()==1? "EleBrake" : "AUX"); return;
      }
    }
    // Special-case RV mode brake mapping: both left & right on implies BRAKE
    if (getUiMode() == 1 && g_relay_on[R_LEFT] && g_relay_on[R_RIGHT] && !g_relay_on[R_TAIL] && !g_relay_on[R_MARKER] && !g_relay_on[R_AUX]) {
      out = "BRAKE"; return;
    }
    out = "RF"; return;
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
  _getStartupGuard(c.getStartupGuard) {}

void DisplayUI::attachTFT(Adafruit_ST7735* tft, int blPin){ _tft=tft; _blPin=blPin; }
void DisplayUI::attachBrightnessSetter(std::function<void(uint8_t)> fn){ _setBrightness=fn; }

void DisplayUI::begin(Preferences& p){
  _prefs = &p;
  if (_blPin >= 0) { pinMode(_blPin, OUTPUT); digitalWrite(_blPin, HIGH); }
  _tft->setTextWrap(false);
  _tft->fillScreen(ST77XX_BLACK);

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

  // Splash
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(10, 38); _tft->print("Swanger Innovations");
  _tft->setTextSize(2);
  _tft->setCursor(26, 58); _tft->print("TLTB");
  delay(900);
  _tft->fillScreen(ST77XX_BLACK);
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

void DisplayUI::rebuildFaultText(){
  _faultText = "";
  auto add=[&](const char* s){
    if (_faultText.length()) _faultText += "  |  ";
    _faultText += s;
  };
  if (_faultMask & FLT_INA_LOAD_MISSING)  add("Load INA missing");
  if (_faultMask & FLT_INA_SRC_MISSING)   add("Src INA missing");
  if (_faultMask & FLT_WIFI_DISCONNECTED) add("Wi-Fi not linked");
  if (_faultMask & FLT_RF_MISSING)        add("RF missing");
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
  // Top MODE row (approx 1.5x via size=2), then Load and Active also at ~1.5x
  const int yMode   = 6;    const int hMode   = 18;   // size=2 text (~16 px high)
  const int yLoad   = yMode + hMode + 2;   const int hLoad   = 18;   // size=2
  const int yActive = yLoad + hLoad + 2;   const int hActive = 18;   // size 1 or 2
  // Removed InputV: place 12V line directly below Active
  const int y12     = yActive + hActive + 2; const int h12 = 12;
  const int yLvp    = y12 + h12 + 2;  const int hLvp    = 12;
  const int yOutv   = yLvp + hLvp + 2; const int hOutv   = 12;
  const int yOcp    = yOutv + hOutv + 2; const int hOcp    = 12;
  const int yHint   = 114;  const int hHint   = 12;

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
      
      // Footer
      _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
      _tft->setCursor(4, yHint);
      _tft->print("OK=Menu  BACK=Home");
    } else {
      // Normal status display
      // Line 1: MODE (top)
      {
        bool selected = (_homeFocus == 1);
        uint16_t bg = selected ? ST77XX_BLUE : ST77XX_BLACK;
        uint16_t fg = ST77XX_WHITE;
        _tft->fillRect(0, yMode-2, W, hMode, bg);
        _tft->setTextSize(2);
        _tft->setTextColor(fg, bg);
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
        // Choose value color: <=20A green, 20-25A yellow, >25A red
  float shownA = fabsf(t.loadA);
  if (shownA > 30.0f) shownA = 30.0f; // cap display at 30A absolute max
        uint16_t valColor = ST77XX_GREEN;            // < 20A
        if (shownA >= 25.0f)      valColor = ST77XX_RED;     // 25–30A (and above)
        else if (shownA >= 20.0f) valColor = ST77XX_YELLOW;  // 20–<25A
        _tft->setTextColor(valColor, ST77XX_BLACK);
        _tft->printf("%4.2f A", shownA);
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

      // Line 4: 12V enable status (shifted up; InputV removed)
      _tft->setTextSize(1);
      _tft->setCursor(4, y12);
      bool en = relayIsOn(R_ENABLE);
      _tft->print("12V sys: "); _tft->print(en?"ENABLED":"DISABLED");

      // Line 5: LVP (shifted up)
      _tft->setTextSize(1);
      _tft->setCursor(4, yLvp);
      bool bypass = _getLvpBypass ? _getLvpBypass() : false;
      if (bypass) {
        _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        _tft->print("LVP : BYPASS");
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      } else {
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        _tft->setCursor(4, yLvp); _tft->print("LVP : ");
        _tft->print(t.lvpLatched? "ACTIVE":"ok");
      }

      // Line 6: Output Voltage status
  _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  _tft->setCursor(4, yOutv);
      _tft->print("OUTV: ");
      bool outvBy = _getOutvBypass ? _getOutvBypass() : false;
      if (outvBy) {
        _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        _tft->print("BYPASS");
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      } else {
        _tft->print(t.outvLatched ? "ACTIVE" : "ok");
      }

  // Next line: OCP status (separate line)
      _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      _tft->setCursor(4, yOcp);
      _tft->print("OCP : ");
      _tft->print(t.ocpLatched ? "ACTIVE" : "ok");

      // Footer
      _tft->setCursor(4, yHint);
      _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
      _tft->print("OK=Menu  BACK=Home");

      drawFaultTicker(true);
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
  static uint8_t s_prevMode = 255; static int s_prevFocus = -1;
  if (s_prevMode != _mode || s_prevFocus != _homeFocus) {
    uint16_t bg = (_homeFocus==1)? ST77XX_BLUE : ST77XX_BLACK;
    _tft->fillRect(0, yMode-2, W, hMode, bg);
    _tft->setTextSize(2);
    _tft->setTextColor(ST77XX_WHITE, bg);
    _tft->setCursor(4, yMode);
    _tft->print("MODE: "); _tft->print(_mode?"RV":"HD");
    s_prevMode = _mode; s_prevFocus = _homeFocus;
  }

  if ((isnan(t.loadA) != isnan(_last.loadA)) ||
      (!isnan(t.loadA) && fabsf(t.loadA - _last.loadA) > 0.02f)) {
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
  if (shownA > 30.0f) shownA = 30.0f; // cap display at 30A absolute max
      uint16_t valColor = ST77XX_GREEN;            // < 20A
      if (shownA >= 25.0f)      valColor = ST77XX_RED;     // 25–30A (and above)
      else if (shownA >= 20.0f) valColor = ST77XX_YELLOW;  // 20–<25A
      _tft->setTextColor(valColor, ST77XX_BLACK);
      _tft->printf("%4.2f A", shownA);
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

  // 12V system state changed? redraw its line (covers cases where InputV didn't change)
  {
    static bool prev12 = false;
    bool en = relayIsOn(R_ENABLE);
    if (en != prev12) {
      // clear the area and redraw the 12V status
      _tft->fillRect(0, y12-2, W, h12+2, ST77XX_BLACK);
  _tft->setTextSize(1);
      _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      _tft->setCursor(4, y12);
      _tft->print("12V sys: "); _tft->print(en?"ENABLED":"DISABLED");
      prev12 = en;
    }
  }

  // LVP status or bypass changed?
  {
  static bool prevBypass = false;
    bool bypass = _getLvpBypass ? _getLvpBypass() : false;
    if ((t.lvpLatched != _last.lvpLatched) || (bypass != prevBypass)) {
      _tft->fillRect(0, yLvp-2, W, hLvp, ST77XX_BLACK);
  _tft->setTextSize(1);
      if (bypass) {
        _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
        _tft->setCursor(4, yLvp); _tft->print("LVP : BYPASS");
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      } else {
        _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        _tft->setCursor(4, yLvp); _tft->print("LVP : ");
        _tft->print(t.lvpLatched? "ACTIVE":"ok");
      }
      prevBypass = bypass;
    }
  }

  // Output Voltage status changed?
  if (t.outvLatched != _last.outvLatched) {
    _tft->fillRect(0, yOutv-2, W, hOutv, ST77XX_BLACK);
    _tft->setTextSize(1);
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    _tft->setCursor(4, yOutv);
    _tft->print("OUTV: ");
    bool outvBy = _getOutvBypass ? _getOutvBypass() : false;
    if (outvBy) {
      _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
      _tft->print("BYPASS");
      _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    } else {
      _tft->print(t.outvLatched ? "ACTIVE" : "ok");
    }
  }

  // OCP status changed?
  if (t.ocpLatched != _last.ocpLatched) {
    _tft->fillRect(0, yOcp-2, W, hOcp, ST77XX_BLACK);
    _tft->setTextSize(1);
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    _tft->setCursor(4, yOcp);
    _tft->print("OCP : ");
    _tft->print(t.ocpLatched ? "ACTIVE" : "ok");
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
  if (!_encOk()) return false;
  uint32_t now = millis();
  if (now - _lastOkMs < 160) return false;  // debounce OK
  _lastOkMs = now;
  return true;
}
bool   DisplayUI::backPressed(){ return _encBack? _encBack():false; }

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
  bool ok   = okPressed();
  bool back = backPressed();

  if (_inMenu) {
    int total = _devMenuOnly ? DEV_MENU_COUNT : MENU_COUNT;
    if (d)   { _menuIdx = (( _menuIdx + d ) % total + total) % total; _needRedraw = true; }
    if (ok)  {
      int srcIdx = _devMenuOnly ? kDevMenuMap[_menuIdx] : _menuIdx;
      handleMenuSelect(srcIdx);
      if (!_devMenuOnly) _inMenu = false; // stay in menu in dev mode
      _needRedraw = true;
    }
    if (back){ if (!_devMenuOnly) { _inMenu = false; _needRedraw = true; } }
  } else {
    if (ok)  {
      if (_homeFocus == 1) { toggleMode(); _needRedraw = true; }
      else { _menuIdx = 0; _inMenu = true; _needRedraw = true; }
    }
    if (back){ _needRedraw = true; }
    if (d) {
      // Toggle focus between none(0) and MODE(1)
      _homeFocus = (_homeFocus + (d>0?1:-1));
      if (_homeFocus < 0) _homeFocus = 1;
      if (_homeFocus > 1) _homeFocus = 0;
      _needRedraw = true;
    }
  }

  // ========== NEW: if we just exited menu/settings, force full home redraw ==========
  if (wasInMenu && !_inMenu) {
    g_forceHomeFull = true;   // next Home draw must be full
    _needRedraw = true;       // ensure we actually draw it this frame
  }
  wasInMenu = _inMenu;

  bool changedHome =
      (!_inMenu) && (
        (isnan(t.loadA) != isnan(_last.loadA)) ||
        (!isnan(t.loadA) && fabsf(t.loadA - _last.loadA) > 0.02f) ||
        (t.lvpLatched != _last.lvpLatched) ||
        (t.ocpLatched != _last.ocpLatched) ||
        _needRedraw
      );

  uint32_t now = millis();
  if (now - _lastMs >= 33) {           // ~30 Hz refresh
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
  if (on) { _inMenu = true; _menuIdx = 0; _needRedraw = true; }
}

// ================================================================
// actions & sub UIs
// ================================================================
void DisplayUI::handleMenuSelect(int idx){
  switch(idx){
    case 0: adjustLvCutoff(); break;                        // Set LVP Cutoff
    case 1: toggleLvpBypass(); break;                       // LVP Bypass
    case 2: adjustOcpLimit(); break;                        // Set OCP Limit
  case 3: adjustOutputVCutoff(); break;                   // Set Output V Cutoff
  case 4: toggleOutvBypass(); break;                      // OutV Bypass
  case 5: {                                               // 12V System
      // 12V System toggle UI (no-flicker incremental updates)
      _tft->fillScreen(ST77XX_BLACK);
      _tft->setTextSize(1);
      _tft->setCursor(6,10); _tft->println("12V System");
      _tft->setCursor(6,56); _tft->print("OK=Toggle  BACK=Exit");

      // Draw state once, then update only when changed
      bool prevEn = !relayIsOn(R_ENABLE); // force initial draw
      auto drawState = [&](){
        bool en = relayIsOn(R_ENABLE);
        _tft->fillRect(0,24,160,14,ST77XX_BLACK); // clear line area around y=30
        _tft->setCursor(6,30); _tft->print("State: "); _tft->print(en?"ENABLED":"DISABLED");
        prevEn = en;
      };

      drawState();

      while(true) {
        bool enNow = relayIsOn(R_ENABLE);
        if (enNow != prevEn) drawState();

        if (okPressed()) {
          if (enNow) relayOff(R_ENABLE); else relayOn(R_ENABLE);
          // brief toast without clearing full screen
          _tft->fillRect(0,44,160,12,ST77XX_BLACK);
          _tft->setCursor(6,44); _tft->print("Toggled");
          delay(250);
          _tft->fillRect(0,44,160,12,ST77XX_BLACK);
          drawState();
        }
        if (backPressed()) break;
        delay(20);
      }
      g_forceHomeFull = true;
    } break;
  case 6: {                                               // Learn RF Button
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
          _tft->print(getUiMode()==1?"REV":"TAIL");
        } else if (s==4) {
          _tft->print("MARKER");
        } else if (s==5) {
          _tft->print(getUiMode()==1?"EleBrake":"AUX");
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
  case 7: {                                               // Clear RF Remotes
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
  case 8: wifiScanAndConnectUI(); break;                  // Wi-Fi Connect
  case 9: wifiForget(); break;                            // Wi-Fi Forget
  case 10: runOta(); break;                               // OTA Update
  case 11: showSystemInfo(); break;                       // System Info
  }
}

// --- adjusters ---
void DisplayUI::saveLvCut(float v){ if(_prefs) _prefs->putFloat(_kLvCut, v); }

void DisplayUI::adjustLvCutoff(){
  _tft->setTextSize(1);
  float v=_prefs->getFloat(_kLvCut, 15.5f);
  _tft->fillScreen(ST77XX_BLACK); _tft->setCursor(6,10); _tft->println("Set LVP Cutoff (V)");
  while(true){
    int8_t d=readStep(); if(d){ v+=d*0.1f; if(v<12)v=12; if(v>20)v=20;
      _tft->fillRect(6,28,148,12,ST77XX_BLACK); _tft->setCursor(6,28); _tft->printf("%4.1f V", v);
    }
    if(okPressed()){ saveLvCut(v); if(_lvChanged) _lvChanged(v); break; }
    if(backPressed()) break;
    delay(8);
  }
}

void DisplayUI::adjustOcpLimit(){
  _tft->setTextSize(1);
  float cur = _prefs->getFloat(KEY_OCP, 20.0f);
  _tft->fillScreen(ST77XX_BLACK); _tft->setCursor(6,10); _tft->println("Set OCP (A)");
  while(true){
    int8_t d=readStep(); if(d){ cur+=d; if(cur<5)cur=5; if(cur>30)cur=30;
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
  float v = _prefs->getFloat(KEY_OUTV_CUTOFF, 11.5f);
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
  _tft->setCursor(6, 112); _tft->print("OK=Clear latch");

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
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,8);  _tft->println("Wi-Fi Connect");
  _tft->setCursor(6,22); _tft->println("Scanning...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  WiFi.setSleep(false);
  delay(120);

  int n = WiFi.scanNetworks();
  if (n <= 0) { _tft->setCursor(6,38); _tft->println("No networks found"); delay(800); g_forceHomeFull = true; return; }

  static String ss; static char sbuf[33];
  auto getter = [&](int i)->const char*{ ss = WiFi.SSID(i); ss.toCharArray(sbuf, sizeof(sbuf)); return sbuf; };
  int pick = listPickerDynamic("Choose SSID", getter, n, 0);
  if (pick < 0) { g_forceHomeFull = true; return; }

  String ssid = WiFi.SSID(pick);
  bool open = (WiFi.encryptionType(pick) == WIFI_AUTH_OPEN);

  String pass;
  if (!open) pass = textInput("Password", "", 63, "abc/ABC/123/sym  OK=sel  BACK=del");

  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,8); _tft->print("Connecting to "); _tft->println(ssid);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis(); int y=28;
  while (WiFi.status()!=WL_CONNECTED && millis()-start < 15000) { _tft->setCursor(6,y); _tft->print("."); delay(200); }

  if (WiFi.status()==WL_CONNECTED) {
    if (_prefs){ _prefs->putString(_kSsid, ssid); _prefs->putString(_kPass, pass); }
    _tft->setCursor(6,y+12); _tft->print("OK: "); _tft->println(WiFi.localIP());
    delay(700);
  } else {
    _tft->setCursor(6,y+12); _tft->println("Failed.");
    delay(700);
  }
  g_forceHomeFull = true;
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
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextSize(1);
  _tft->setCursor(6,10); _tft->println("OTA Update");

  Ota::Callbacks cb;
  cb.onStatus = [&](const char* s){
    // Show status/messages at y=28 or y=92 for final
    _tft->setCursor(6,28); _tft->fillRect(6,28,148,12,ST77XX_BLACK);
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    _tft->print(s);
  };
  cb.onProgress = [&](size_t w, size_t t){
    _tft->fillRect(6,60,148,10,ST77XX_BLACK);
    _tft->setCursor(6,60);
    if (t) _tft->printf("%u/%u", (unsigned)w, (unsigned)t);
    else   _tft->printf("%u", (unsigned)w);
  };

  // Use default repo from build flag OTA_REPO; pass nullptr to use fallback
  bool ok = Ota::updateFromGithubLatest(nullptr, cb);
  if (!ok) {
    _tft->setCursor(6,92); _tft->println("OTA failed");
    delay(900);
    g_forceHomeFull = true;
  }
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
