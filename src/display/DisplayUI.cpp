#include "DisplayUI.hpp"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <math.h>

#include "pins.hpp"
#include "prefs.hpp"

#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>

// ---------------- Menu ----------------
static const char* const kMenuItems[] = {
  "Wi-Fi Connect",
  "Wi-Fi Forget",
  "OTA Update",
  "Brightness",
  "Set LVP Cutoff",
  "LVP Bypass",
  "Set OCP Limit",
  "Scan All Outputs",
  "Learn RF Button",
  "About",
  "System Info"
};
static constexpr int MENU_COUNT = sizeof(kMenuItems) / sizeof(kMenuItems[0]);

static const char* const OTA_URL_KEY = "ota_url";

// Relay labels
static const char* relayNameByIdx(int idx){
  switch(idx){
    case 0: return "LEFT";
    case 1: return "RIGHT";
    case 2: return "BRAKE";
    case 3: return "TAIL";
    case 4: return "MARKER";
    case 5: return "AUX";
    default: {
      static char buf[8];
      snprintf(buf, sizeof(buf), "R%d", idx+1);
      return buf;
    }
  }
}

static void getActiveRelayStatus(String& out){
  const int RELAY_COUNT = (int)(sizeof(RELAY_PIN)/sizeof(RELAY_PIN[0]));
  int activeCount = 0, lastIdx = -1;
  for(int i=0;i<RELAY_COUNT;i++){
    if (digitalRead(RELAY_PIN[i]) == HIGH){ activeCount++; lastIdx = i; }
  }
  if (activeCount == 0) out = "None";
  else if (activeCount == 1) out = relayNameByIdx(lastIdx);
  else out = String("Multiple (") + activeCount + ")";
}

// ================================================================
// ctor / setup
// ================================================================
DisplayUI::DisplayUI(const DisplayCtor& c)
: _pins(c.pins),
  _ns(c.ns),
  _kBright(c.kBright),
  _kLvCut(c.kLvCut),
  _kSsid(c.kWifiSsid),
  _kPass(c.kWifiPass),
  _readSrcV(c.readSrcV),
  _readLoadA(c.readLoadA),
  _scanAll(c.scanAll),
  _otaStart(c.onOtaStart),
  _otaEnd(c.onOtaEnd),
  _lvChanged(c.onLvCutChanged),
  _ocpChanged(c.onOcpChanged),
  _rfLearn(c.onRfLearn),
  _getLvpBypass(c.getLvpBypass),
  _setLvpBypass(c.setLvpBypass) {}

void DisplayUI::attachTFT(Adafruit_ST7735* tft, int blPin){ _tft=tft; _blPin=blPin; }
void DisplayUI::attachBrightnessSetter(std::function<void(uint8_t)> fn){ _setBrightness=fn; }

void DisplayUI::begin(Preferences& p){
  _prefs = &p;
  if (_blPin >= 0) { pinMode(_blPin, OUTPUT); digitalWrite(_blPin, HIGH); }
  _tft->setTextWrap(false);
  _tft->fillScreen(ST77XX_BLACK);

  // Apply persisted brightness
  uint8_t bri = _prefs->getUChar(_kBright, 255);
  if (_setBrightness) _setBrightness(bri);

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
// home / menu draw
// ================================================================
void DisplayUI::showStatus(const Telemetry& t){
  _tft->fillScreen(ST77XX_BLACK);

  // Line 1: Load (size 2)
  _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  _tft->setTextSize(2);
  _tft->setCursor(4, 6);
  if (isnan(t.loadA)) _tft->print("Load:  N/A");
  else                _tft->printf("Load: %4.2f A", t.loadA);

  // Line 2: Active Relay (size 2)
  String ar; getActiveRelayStatus(ar);
  _tft->setCursor(4, 26);
  _tft->printf("Active: %s", ar.c_str());

  // Line 3: SrcV (size 1)
  _tft->setTextSize(1);
  _tft->setCursor(4, 50);
  if (isnan(t.srcV)) _tft->print("SrcV:  N/A");
  else               _tft->printf("SrcV: %4.2f V", t.srcV);

  // Line 4: LVP (size 1)
  _tft->setCursor(4, 64);
  bool bypass = _getLvpBypass ? _getLvpBypass() : false;
  if (bypass) {
    _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
    _tft->print("LVP : BYPASS");
    _tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  } else {
    _tft->print("LVP : ");
    _tft->print(t.lvpLatched? "ACTIVE":"ok");
  }

  // Footer hint
  _tft->setCursor(4, 98);
  _tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
  _tft->print("OK=Menu  BACK=Home");

  drawFaultTicker(true);

  _last=t; _needRedraw=false;
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

  // function-static window state
  static int menuTop = 0;      // first visible index
  static int prevTop = -1;     // previously drawn top
  static int prevIdx = -1;     // previously highlighted index

  // 🔧 Force a full repaint when caller asked for a redraw
  // (e.g., when you press OK on Home to enter the menu)
  if (_needRedraw) { prevTop = -1; prevIdx = -1; }

  auto drawRow = [&](int i, bool sel){
    if (i < menuTop || i >= menuTop + rows) return;
    int y = y0 + (i - menuTop) * rowH;
    uint16_t bg = sel ? ST77XX_BLUE : ST77XX_BLACK;
    _tft->fillRect(0, y-2, 160, rowH, bg);
    _tft->setTextColor(ST77XX_WHITE, bg);
    _tft->setCursor(6, y);
    _tft->print(kMenuItems[i]);
  };

  // Keep selection in view
  if (_menuIdx < menuTop) menuTop = _menuIdx;
  if (_menuIdx >= menuTop + rows) menuTop = _menuIdx - rows + 1;

  // First paint or window changed → repaint the visible window
  if (prevTop != menuTop || prevIdx < 0) {
    _tft->fillScreen(ST77XX_BLACK);
    for (int i = menuTop; i < menuTop + rows && i < MENU_COUNT; ++i) {
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
  _tft->print(kMenuItems[i]);
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
  int8_t d  = readStep();
  bool ok   = okPressed();
  bool back = backPressed();

  if (_inMenu) {
    if (d)   { _menuIdx = (( _menuIdx + d ) % MENU_COUNT + MENU_COUNT) % MENU_COUNT; _needRedraw = true; }
    if (ok)  { handleMenuSelect(_menuIdx); _inMenu = false; _needRedraw = true; }
    if (back){ _inMenu = false; _needRedraw = true; }
  } else {
    if (ok)  { _menuIdx = 0; _inMenu = true; _needRedraw = true; }
    if (back){ _needRedraw = true; }
  }

  bool changedHome =
      (!_inMenu) && (
        (isnan(t.srcV)  != isnan(_last.srcV))  ||
        (isnan(t.loadA) != isnan(_last.loadA)) ||
        (!isnan(t.srcV)  && fabsf(t.srcV  - _last.srcV ) > 0.02f) ||
        (!isnan(t.loadA) && fabsf(t.loadA - _last.loadA) > 0.02f) ||
        (t.lvpLatched != _last.lvpLatched) ||
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

// ================================================================
// actions & sub UIs
// ================================================================
void DisplayUI::handleMenuSelect(int idx){
  switch(idx){
    case 0: wifiScanAndConnectUI(); break;
    case 1: wifiForget(); break;
    case 2: runOta(); break;
    case 3: adjustBrightness(); break;
    case 4: adjustLvCutoff(); break;
    case 5: toggleLvpBypass(); break;
    case 6: adjustOcpLimit(); break;
    case 7: if(_scanAll) _scanAll(); break;
    case 8: {
      // RF Learn (no flicker): draw once, update only changed line
      int sel = 0, lastSel = -1;
      _tft->fillScreen(ST77XX_BLACK);
      _tft->setCursor(6,8);  _tft->print("Learn RF for:");
      _tft->setCursor(6,44); _tft->print("OK=Start  BACK=Exit");

      auto drawSel = [&](int s){
        _tft->fillRect(0,20,160,16,ST77XX_BLACK);
        _tft->setCursor(6,24);
        _tft->print(s==0?"LEFT":s==1?"RIGHT":s==2?"BRAKE":s==3?"TAIL":s==4?"MARKER":"AUX");
      };

      drawSel(sel);
      while(true){
        int8_t dd=readStep();
        if(dd){
          sel = ((sel + dd) % 6 + 6) % 6;
        }
        if (sel != lastSel) { drawSel(sel); lastSel = sel; }

        if(okPressed()){
          _tft->fillRect(0,60,160,14,ST77XX_BLACK);
          _tft->setCursor(6,60); _tft->print("Listening...");
          bool ok = _rfLearn ? _rfLearn(sel) : false;
          _tft->fillRect(0,60,160,14,ST77XX_BLACK);
          _tft->setCursor(6,60); _tft->print(ok ? "Saved" : "Failed");
          // stay here until BACK so user can see result
          _tft->setCursor(6,76); _tft->print("BACK=Exit");
          while(!backPressed()) delay(10);
          break;
        }
        if(backPressed()) break;
        delay(12);
      }
    } break;
    case 9: // About
      _tft->fillScreen(ST77XX_BLACK);
      _tft->setCursor(6,10);
      _tft->println("Swanger Innovations\nTLTB");
      // Wait for BACK (keeps page visible)
      _tft->setCursor(6,36); _tft->println("BACK=Exit");
      while(!backPressed()) delay(10);
      break;
    case 10: showSystemInfo(); break;
  }
}

// --- adjusters ---
void DisplayUI::saveBrightness(uint8_t v){ if(_prefs) _prefs->putUChar(_kBright, v); }
void DisplayUI::saveLvCut(float v){ if(_prefs) _prefs->putFloat(_kLvCut, v); }

void DisplayUI::adjustBrightness(){
  uint8_t v=_prefs->getUChar(_kBright, 255);
  _tft->fillScreen(ST77XX_BLACK); _tft->setCursor(6,10); _tft->println("Brightness");
  _tft->setCursor(6,28); _tft->printf("%3u/255", v);
  while(true){
    int8_t d=readStep(); if(d){
      int nv = (int)v + d*8; if(nv<0) nv=0; if(nv>255) nv=255; v=(uint8_t)nv;
      _tft->fillRect(6,28,120,12,ST77XX_BLACK); _tft->setCursor(6,28); _tft->printf("%3u/255", v);
      if(_setBrightness) _setBrightness(v);
    }
    if(okPressed()){ saveBrightness(v); break; }
    if(backPressed()) break;
    delay(8);
  }
}

void DisplayUI::adjustLvCutoff(){
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
  float cur = _prefs->getFloat(KEY_OCP, 20.0f);
  _tft->fillScreen(ST77XX_BLACK); _tft->setCursor(6,10); _tft->println("Set OCP (A)");
  while(true){
    int8_t d=readStep(); if(d){ cur+=d; if(cur<5)cur=5; if(cur>40)cur=40;
      _tft->fillRect(6,28,148,12,ST77XX_BLACK); _tft->setCursor(6,28); _tft->printf("%4.1f A", cur);
    }
    if(okPressed()){ if(_ocpChanged) _ocpChanged(cur); _prefs->putFloat(KEY_OCP, cur); break; }
    if(backPressed()) break;
    delay(8);
  }
}

void DisplayUI::toggleLvpBypass(){
  bool on = _getLvpBypass ? _getLvpBypass() : false;
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setCursor(6,10); _tft->println("LVP Bypass");
  _tft->setCursor(6,28); _tft->print("State: ");
  _tft->print(on ? "ON" : "OFF");
  _tft->setCursor(6,44); _tft->println("OK=Toggle  BACK=Exit");

  while(true){
    int8_t d=readStep(); (void)d;
    if (okPressed()) { if (_setLvpBypass) _setLvpBypass(!on); break; }
    if (backPressed()) break;
    delay(10);
  }
}

// ================================================================
// Scan UI (used by main.cpp) — now holds on screen after done
// ================================================================
void DisplayUI::showScanBegin() {
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setCursor(6,10);
  _tft->println("Scanning relays...");
  _tft->setCursor(6, 116); _tft->print("BACK=Exit");
}

void DisplayUI::showScanResult(int idx, const char* res) {
  int y = 28 + idx * 12; 
  if (y > 110) y = 110; // clamp
  _tft->setCursor(6, y);
  _tft->printf("R%d: %s\n", idx + 1, res ? res : "-");
}

void DisplayUI::showScanDone() {
  _tft->setCursor(6, 100);
  _tft->print("Done - BACK=Exit");
  // Hold here until user presses BACK
  while(!backPressed()) delay(10);
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
  _tft->setCursor(6, 112); _tft->print("OK=Clear latch   BACK=Ignore");

  while (true) {
    if (okPressed())   return true;
    if (backPressed()) return false;
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
  int idx = startIdx < 0 ? 0 : (startIdx >= count ? count - 1 : startIdx);
  int top = 0;

  _tft->fillScreen(ST77XX_BLACK);
  _tft->setTextColor(ST77XX_CYAN, ST77XX_BLACK);
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
    if (okPressed())   return idx;
    if (backPressed()) return -1;
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
        else if (sel == 6) { return buf; }

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
      else { return buf; }
    }

    delay(10);
  }
}

// Wi-Fi flow
void DisplayUI::wifiScanAndConnectUI(){
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setCursor(6,8);  _tft->println("Wi-Fi Connect");
  _tft->setCursor(6,22); _tft->println("Scanning...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  WiFi.setSleep(false);
  delay(120);

  int n = WiFi.scanNetworks();
  if (n <= 0) { _tft->setCursor(6,38); _tft->println("No networks found"); delay(800); return; }

  static String ss; static char sbuf[33];
  auto getter = [&](int i)->const char*{ ss = WiFi.SSID(i); ss.toCharArray(sbuf, sizeof(sbuf)); return sbuf; };
  int pick = listPickerDynamic("Choose SSID", getter, n, 0);
  if (pick < 0) return;

  String ssid = WiFi.SSID(pick);
  bool open = (WiFi.encryptionType(pick) == WIFI_AUTH_OPEN);

  String pass;
  if (!open) pass = textInput("Password", "", 63, "abc/ABC/123/sym  OK=sel  BACK=del");

  _tft->fillScreen(ST77XX_BLACK);
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
}

void DisplayUI::wifiForget(){
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setCursor(6,10); _tft->println("Wi-Fi Forget...");
  if (_prefs) { _prefs->remove(_kSsid); _prefs->remove(_kPass); }
  WiFi.disconnect(true, true);
  delay(250);
  _tft->setCursor(6,28); _tft->println("Done");
  delay(500);
}

// OTA
void DisplayUI::runOta(){
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setCursor(6,10); _tft->println("OTA Update");

  String url = _prefs ? _prefs->getString(OTA_URL_KEY, "") : "";
  if (url.length()==0) {
    url = textInput("Set OTA URL", "http://", 120, "Grid keyboard: OK=add, BACK=del, done");
    if (url.length()==0){ _tft->setCursor(6,28); _tft->println("No URL set."); delay(700); return; }
    if (_prefs) _prefs->putString(OTA_URL_KEY, url);
  }

  if (WiFi.status()!=WL_CONNECTED) { _tft->setCursor(6,28); _tft->println("Wi-Fi not connected"); delay(700); return; }

  HTTPClient http; http.setTimeout(8000);
  if (!http.begin(url)) { _tft->setCursor(6,46); _tft->println("Bad URL"); delay(700); return; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { _tft->setCursor(6,46); _tft->printf("HTTP %d\n", code); http.end(); delay(800); return; }

  int len = http.getSize();
  WiFiClient *stream = http.getStreamPtr();

  if (!Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN)) { _tft->setCursor(6,62); _tft->println("Update.begin fail"); http.end(); delay(800); return; }

  _tft->setCursor(6,46); _tft->println("Downloading...");
  size_t written = 0; uint8_t buff[2048]; uint32_t lastDraw=0;
  while (http.connected() && (len > 0 || len == -1)) {
    size_t avail = stream->available();
    if (avail) {
      int c = stream->readBytes(buff, (avail > sizeof(buff)) ? sizeof(buff) : avail);
      if (c < 0) break;
      if (Update.write(buff, c) != (size_t)c) { _tft->setCursor(6,62); _tft->println("Write error"); Update.abort(); http.end(); delay(800); return; }
      written += c;
      uint32_t now = millis();
      if (now - lastDraw > 120) {
        _tft->fillRect(6,60,148,10,ST77XX_BLACK);
        _tft->setCursor(6,60);
        if (len > 0) _tft->printf("%u/%d", (unsigned)written, len);
        else         _tft->printf("%u", (unsigned)written);
        lastDraw = now;
      }
    } else {
      delay(1);
    }
    if (len > 0) len -= avail;
  }
  http.end();

  if (!Update.end(true)) { _tft->setCursor(6,76); _tft->printf("End err %u", Update.getError()); delay(1000); return; }

  _tft->setCursor(6,76); _tft->println("OTA OK. Reboot...");
  delay(700);
  ESP.restart();
}

// ================================================================
// Info page
// ================================================================
void DisplayUI::showSystemInfo(){
  _tft->fillScreen(ST77XX_BLACK);
  _tft->setCursor(4, 6);  _tft->setTextColor(ST77XX_CYAN); _tft->println("System Info & Faults");
  _tft->setTextColor(ST77XX_WHITE);

  int y=22;
  auto line=[&](const char* k, const char* v){ _tft->setCursor(4,y); _tft->print(k); _tft->print(": "); _tft->println(v); y+=12; };

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
}
