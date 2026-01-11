// File Overview: Application entry point for the Trailer Lighting Test Box; initializes
// hardware, drives the UI, samples telemetry, and coordinates protection plus relay logic.
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_ST7735.h>

// ESP-IDF C headers already provide their own extern "C" guards; direct includes keep this cleaner.
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"

#include <WiFi.h>

#include "pins.hpp"
#include "prefs.hpp"
#include "display/DisplayUI.hpp"
#include "sensors/INA226.hpp"
#include "rf/RF.hpp"
#include "buzzer.hpp"
#include "relays.hpp"
#include <Preferences.h>
#include "power/Protector.hpp"

// ---------------- Globals ----------------
static Adafruit_ST7735* tft = nullptr;   // shared SPI
static DisplayUI* ui = nullptr;
Preferences prefs;
static Telemetry tele{};
// OTA rollback verification
static bool g_otaPendingVerify = false;
static uint32_t g_otaBootMs = 0;
static bool g_devBoot = false;   // Developer-boot mode flag
static bool g_startupGuard = false; // Prevents relay activation until 1p8t is cycled to OFF

// Cooldown timer state (20A usage limit)
static uint32_t g_highCurrentStartMs = 0; // When >20A current started (0=not active)
static uint32_t g_cooldownStartMs = 0;    // When cooldown period started (0=not in cooldown)
static constexpr uint32_t HIGH_CURRENT_LIMIT_MS = 120000; // 120 seconds
static constexpr uint32_t COOLDOWN_PERIOD_MS = 120000;     // 120 seconds
static constexpr float HIGH_CURRENT_THRESHOLD = 20.0f;     // 20 amps

// LEDC (backlight)
static const int BL_CHANNEL = 0;

// Backlight
static void setBacklight(uint8_t v){ ledcWrite(BL_CHANNEL, v); }

// -------------------------------------------------------------------
// ----------- Encoder (ISR-based, fast + debounced) -----------------
// -------------------------------------------------------------------
static volatile int32_t enc_delta = 0;
static volatile uint32_t enc_last_us = 0;
static constexpr uint32_t ENC_ISR_DEADTIME_US = 150; // adjust 120–220us if needed

void IRAM_ATTR enc_isrA() {
  uint32_t now = micros();
  if ((uint32_t)(now - enc_last_us) < ENC_ISR_DEADTIME_US) return;
  enc_last_us = now;
  int b = digitalRead(PIN_ENC_B);      // direction at A's rising edge
  enc_delta += (b ? -1 : +1);
}

static int8_t readEncoderStep() {
  noInterrupts();
  int32_t d = enc_delta;
  enc_delta = 0;
  interrupts();
  if (d > 3) d = 3;                     // smooth fast spins
  if (d < -3) d = -3;
  return (int8_t)d;
}

static bool okPressedEdge(){
  static bool last=false;
  bool cur = (digitalRead(PIN_ENC_OK) == ENC_OK_ACTIVE_LEVEL);
  bool edge = (cur && !last);
  last = cur;
  return edge;
}
static bool backPressed(){ return (digitalRead(PIN_ENC_BACK) == LOW); }

// ---------------- Rotary selector ----------------
enum RotaryMode {
  MODE_ALL_OFF = 0,   // P1
  MODE_RF_ENABLE,     // P2
  MODE_LEFT,          // P3
  MODE_RIGHT,         // P4
  MODE_BRAKE,         // P5
  MODE_TAIL,          // P6
  MODE_MARKER,        // P7
  MODE_AUX            // P8
};
// Track stable rotary mode to avoid false triggers when switch is between detents
static RotaryMode g_stableRotaryMode = MODE_ALL_OFF;

static RotaryMode readRotary() {
  // Inputs are PULLUP, so LOW = active position
  if (digitalRead(PIN_ROT_P1) == LOW) return MODE_ALL_OFF;
  if (digitalRead(PIN_ROT_P2) == LOW) return MODE_RF_ENABLE;
  if (digitalRead(PIN_ROT_P3) == LOW) return MODE_LEFT;
  if (digitalRead(PIN_ROT_P4) == LOW) return MODE_RIGHT;
  if (digitalRead(PIN_ROT_P5) == LOW) return MODE_BRAKE;
  if (digitalRead(PIN_ROT_P6) == LOW) return MODE_TAIL;
  if (digitalRead(PIN_ROT_P7) == LOW) return MODE_MARKER;
  if (digitalRead(PIN_ROT_P8) == LOW) return MODE_AUX;
  return MODE_ALL_OFF; // fallback if between detents or no input
}

static void enforceRotaryMode(RotaryMode m) {
  // Startup guard: keep all relays OFF until 1p8t is cycled to OFF position
  if (g_startupGuard) {
    // Clear guard when rotary is moved to OFF position
    if (m == MODE_ALL_OFF) {
      g_startupGuard = false;
    }
    // Keep all relays OFF while guard is active
    for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
    return;
  }

  // Normal operation: In all non-RF modes, we *force* the relay states each loop.
  // This guarantees RF is effectively ignored unless in MODE_RF_ENABLE.
  auto allOff = [](){
    // Turn off all output relays, but NOT R_ENABLE (system master relay)
    for (int i = 0; i < (int)R_ENABLE; ++i) relayOff(i);
  };

  switch (m) {
    case MODE_ALL_OFF:
      allOff();
      break;

    case MODE_RF_ENABLE:
      // Do not force anything; RF subsystem may control relays.
      break;

    case MODE_LEFT:
      allOff(); relayOn(R_LEFT);
      break;

    case MODE_RIGHT:
      allOff(); relayOn(R_RIGHT);
      break;

    case MODE_BRAKE:
      allOff();
      if (getUiMode() == 1) { // RV
        relayOn(R_LEFT); relayOn(R_RIGHT);
      } else {
        relayOn(R_BRAKE);
      }
      break;

    case MODE_TAIL:
      allOff(); relayOn(R_TAIL);
      break;

    case MODE_MARKER:
      allOff(); relayOn(R_MARKER);
      break;

    case MODE_AUX:
      allOff();
      if (getUiMode() == 1) { // RV
        relayOn(R_BRAKE);
      } else {
        relayOn(R_AUX);
      }
      break;
  }

  // Relay 7 (R_ENABLE) must be OFF when the selector is in position 1 (ALL_OFF)
  // and ON in all other positions. This is independent of RF control for the
  // other relays — we enforce it here.
  if (m == MODE_ALL_OFF) {
    relayOff(R_ENABLE);
  } else {
    relayOn(R_ENABLE);
  }
}

// (Relay scan feature removed)

// ----------- Faults -----------
static uint32_t computeFaultMask(){
  uint32_t m = FLT_NONE;
  if (!INA226::PRESENT)     m |= FLT_INA_LOAD_MISSING;
  if (!INA226_SRC::PRESENT) m |= FLT_INA_SRC_MISSING;

  // Only flag Wi-Fi if creds exist
  bool haveCreds = prefs.getString(KEY_WIFI_SSID, "").length() > 0;
  if (haveCreds && WiFi.status() != WL_CONNECTED) m |= FLT_WIFI_DISCONNECTED;

  if (!RF::isPresent())     m |= FLT_RF_MISSING;
  return m;
}

// ---------------- setup/loop ----------------
void setup() {
  esp_task_wdt_deinit();

  // TFT & encoder/buttons pins
  // Keep backlight OFF until panel is fully initialized to avoid white-screen on cold power
  pinMode(PIN_TFT_BL, OUTPUT); digitalWrite(PIN_TFT_BL, LOW);
  pinMode(PIN_TFT_CS, OUTPUT);  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_RST, OUTPUT);
  // Allow power rails to settle on cold battery connect
  delay(60);

  // Check if this app is booting in pending-verify state (OTA rollback flow)
  {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
      esp_ota_img_states_t st;
      if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        g_otaPendingVerify = true;
        g_otaBootMs = millis();
      }
    }
  }
  pinMode(PIN_ENC_A,    INPUT_PULLUP);
  pinMode(PIN_ENC_B,    INPUT_PULLUP);
  pinMode(PIN_ENC_OK,   INPUT_PULLUP); // OK: idle HIGH (~3V3), pressed LOW
  pinMode(PIN_ENC_BACK, INPUT_PULLUP);

  // Dev boot detection: BACK button held during power-on (simplified)
  // Use a short settling delay then sample BACK; no rotary position required.
  delay(5);
  if (digitalRead(PIN_ENC_BACK) == LOW) {
    g_devBoot = true;
    // Wait for BACK release so the UI menu is not immediately closed by a held button
    while (digitalRead(PIN_ENC_BACK) == LOW) {
      delay(1);
    }
  }

  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), enc_isrA, RISING);

  // Rotary switch pins
  pinMode(PIN_ROT_P1, INPUT_PULLUP);
  pinMode(PIN_ROT_P2, INPUT_PULLUP);
  pinMode(PIN_ROT_P3, INPUT_PULLUP);
  pinMode(PIN_ROT_P4, INPUT_PULLUP);
  pinMode(PIN_ROT_P5, INPUT_PULLUP);
  pinMode(PIN_ROT_P6, INPUT_PULLUP);
  pinMode(PIN_ROT_P7, INPUT_PULLUP);
  pinMode(PIN_ROT_P8, INPUT_PULLUP);

  // Startup guard: if 1p8t is not in OFF position (P1), require cycling to OFF first
  delay(10); // Allow pins to settle
  if (digitalRead(PIN_ROT_P1) != LOW) {
    g_startupGuard = true; // Guard is active until cycled to OFF
  }

  // Relays safe init
  relaysBegin();

  // SPI once (shared TFT + RF)
  // Explicitly configure SPI pins before begin to ensure JTAG defaults are overridden
  pinMode(PIN_FSPI_SCK, OUTPUT);
  pinMode(PIN_FSPI_MOSI, OUTPUT);
  pinMode(PIN_FSPI_MISO, INPUT);
  SPI.begin(PIN_FSPI_SCK, PIN_FSPI_MISO, PIN_FSPI_MOSI, PIN_TFT_CS);
  delay(30); // allow peripheral settle

  // TFT reset + init
  // Stronger hardware reset timing to improve first-boot reliability after long power-off
  digitalWrite(PIN_TFT_RST, HIGH); delay(50);
  digitalWrite(PIN_TFT_RST, LOW ); delay(120);
  digitalWrite(PIN_TFT_RST, HIGH); delay(150);

  tft = new Adafruit_ST7735(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
  // Start with a conservative SPI speed for signal integrity on longer jumpers
  tft->setSPISpeed(8000000UL);
  tft->initR(INITR_BLACKTAB);
  tft->setRotation(1);
  tft->fillScreen(ST77XX_BLACK);

  // Backlight (8-bit)
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(PIN_TFT_BL, BL_CHANNEL);
  // Enable backlight only after panel is initialized and cleared
  ledcWrite(BL_CHANNEL, 255);

  // prefs first
  prefs.begin(NVS_NS, false);

  // Dev-boot now uses existing UI Wi‑Fi/OTA pages; no special flow here.

  // UI wire-up
  ui = new DisplayUI(DisplayCtor{
    .pins        = {PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BL},
    .ns          = NVS_NS,
    .kLvCut      = KEY_LV_CUTOFF,
    .kWifiSsid   = KEY_WIFI_SSID,
    .kWifiPass   = KEY_WIFI_PASS,
    .readSrcV    = [](){ return INA226_SRC::readBusV(); },
    .readLoadA   = [](){ return INA226::readCurrentA(); },
    .onOtaStart  = nullptr,
    .onOtaEnd    = nullptr,
    // Apply new LVP cutoff immediately to protector
    .onLvCutChanged = [](float v){ protector.setLvpCutoff(v); },
  .onOcpChanged   = [](float a){ protector.setOcpLimit(a); },
  .onOutvChanged  = [](float v){ protector.setOutvCutoff(v); },
  .getOutvBypass  = [](){ return protector.outvBypass(); },
  .setOutvBypass  = [](bool on){ protector.setOutvBypass(on); },
    .onRfLearn      = [](int idx){ return RF::learn(idx); },
    .getLvpBypass   = [](){ return protector.lvpBypass(); },
    .setLvpBypass   = [](bool on){ protector.setLvpBypass(on); },
    .getStartupGuard = [](){ return g_startupGuard; },
  });
  ui->attachTFT(tft, PIN_TFT_BL);
  ui->attachBrightnessSetter(setBacklight);
  ui->setEncoderReaders(readEncoderStep, okPressedEdge, backPressed);

  ui->begin(prefs);               // shows splash, applies brightness

  if (g_devBoot) {
    ui->setFaultMask(0);
    ui->enterMenu();
  }

  // sensors + RF (skip in dev-boot)
  if (!g_devBoot) {
    INA226::begin();
    INA226_SRC::begin();
    RF::begin();
    Buzzer::begin();
  } else {
    Buzzer::begin(); // keep buzzer available for feedback if needed
  }

  // Auto-join Wi-Fi (non-blocking)
  {
    String ssid = prefs.getString(KEY_WIFI_SSID, "");
    if (ssid.length()) {
      String pass = prefs.getString(KEY_WIFI_PASS, "");
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.begin(ssid.c_str(), pass.c_str());
    }
  }

  // Protector init (loads thresholds)
  if (!g_devBoot) {
    protector.begin(&prefs);
    ui->setFaultMask(computeFaultMask());
    ui->showStatus(tele);
    
    // Boot-time off-current safety check: wait 1s for system to stabilize, then verify no unexpected load
    delay(1000);
    // Read current after stabilization
    float bootCurrent = INA226::PRESENT ? INA226::readCurrentA() : 0.0f;
    if (!isnan(bootCurrent) && bootCurrent > 2.0f) {
      // Unexpected current draw at boot - critical safety issue
      tft->fillScreen(ST77XX_BLACK);
      tft->setTextColor(ST77XX_RED);
      tft->setTextSize(2);
      tft->setCursor(10, 40);
      tft->println("UNEXPECTED");
      tft->setCursor(10, 60);
      tft->println("CURRENT DRAW!");
      tft->setTextSize(1);
      tft->setTextColor(ST77XX_WHITE);
      tft->setCursor(10, 90);
      tft->println("Power off and remove");
      tft->setCursor(10, 100);
      tft->println("battery NOW!");
      tft->setCursor(10, 120);
      tft->printf("Boot current: %.1fA", bootCurrent);
      // Block here forever - require power cycle
      while(true) {
        delay(1000);
      }
    }
  }
}

void loop() {
  if (g_devBoot) {
    tele.srcV = NAN;
    tele.loadA = NAN;
    tele.outV = NAN;
    tele.lvpLatched = false;
    tele.ocpLatched = false;
    tele.outvLatched = false;
    ui->setFaultMask(0);
    ui->tick(tele);
    delay(10);
    return;
  }

  // Read telemetry if present
  tele.srcV  = INA226_SRC::PRESENT ? INA226_SRC::readBusV()    : NAN;
  tele.loadA = INA226::PRESENT     ? INA226::readCurrentA()    : NAN;
  tele.outV  = INA226::PRESENT     ? INA226::readBusV()        : NAN; // LOAD INA226 bus voltage as buck output

  // Protection logic
  // Ensure OCP hold engages before tick so auto-clear cannot occur while rotating toward OFF
  if (protector.isOcpLatched()) {
    protector.setOcpHold(true);
  } else {
    protector.setOcpHold(false);
  }
  protector.tick(tele.srcV, tele.loadA, tele.outV, millis());
  // Track latches separately for UI clarity
  tele.lvpLatched   = protector.isLvpLatched();
  tele.ocpLatched   = protector.isOcpLatched();
  tele.outvLatched  = protector.isOutvLatched();

  // Cooldown timer logic: limit sustained high current usage
  uint32_t now = millis();
  float current = !isnan(tele.loadA) ? fabsf(tele.loadA) : 0.0f;
  
  if (g_cooldownStartMs > 0) {
    // Currently in cooldown period - keep enable relay OFF
    relayOff(R_ENABLE);
    uint32_t elapsed = now - g_cooldownStartMs;
    if (elapsed >= COOLDOWN_PERIOD_MS) {
      // Cooldown complete - resume normal operation
      g_cooldownStartMs = 0;
      g_highCurrentStartMs = 0;
      tele.cooldownSecsRemaining = 0;
      tele.cooldownActive = false;
    } else {
      // Still cooling down - update countdown
      tele.cooldownSecsRemaining = (COOLDOWN_PERIOD_MS - elapsed) / 1000 + 1;
      tele.cooldownActive = true;
    }
  } else if (current > HIGH_CURRENT_THRESHOLD) {
    // Current is high - track duration
    if (g_highCurrentStartMs == 0) {
      g_highCurrentStartMs = now; // Start timing high current
    } else {
      uint32_t highDuration = now - g_highCurrentStartMs;
      if (highDuration >= HIGH_CURRENT_LIMIT_MS) {
        // Exceeded time limit - enter cooldown
        g_cooldownStartMs = now;
        g_highCurrentStartMs = 0;
        tele.cooldownSecsRemaining = COOLDOWN_PERIOD_MS / 1000;
        tele.cooldownActive = true;
        relayOff(R_ENABLE); // Immediately disable
      } else {
        // Still within limit - show countdown to limit
        tele.cooldownSecsRemaining = (HIGH_CURRENT_LIMIT_MS - highDuration) / 1000 + 1;
        tele.cooldownActive = false;
      }
    }
  } else {
    // Current dropped below threshold - reset high current timer
    g_highCurrentStartMs = 0;
    tele.cooldownSecsRemaining = 0;
    tele.cooldownActive = false;
  }
  // Buzzer fault pattern tick (priority over one-shot)
  // Suppress buzzer for LVP/OUTV when those protections are bypassed
  {
    bool beepFault = false;
    if (tele.ocpLatched) beepFault = true; // OCP always beeps (no bypass)
    if (tele.lvpLatched && !protector.lvpBypass()) beepFault = true;
    if (tele.outvLatched && !protector.outvBypass()) beepFault = true;
    if (ui && ui->menuActive()) {
      beepFault = false; // silence buzzer whenever settings menu is on screen
    }
    Buzzer::tick(beepFault, millis());
  }

  // OCP modal (single-shot per continuous fault; re-armed after healthy period)
  static bool     ocpAcked = false;           // has the current OCP fault cycle been acknowledged?
  static uint32_t ocpHealthySince = 0;        // ms timestamp when OCP became healthy (unlatched)
  {
    bool ocpLatched = protector.isOcpLatched();
    if (ocpLatched) {
      // Require user to return 1P8T to OFF before allowing 12V enable again
      g_startupGuard = true;
      // Always hold OCP while latched so it cannot auto-clear until OFF is selected
      protector.setOcpHold(true);
      ocpHealthySince = 0; // fault persists; not healthy
      if (!ocpAcked) {
        // Show a blocking modal that cannot be cleared with OK; require OFF cycle.
        if (tft) {
          tft->fillScreen(ST77XX_RED);
          tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
          tft->setTextSize(2);
          tft->setCursor(6, 6);  tft->print("OCP TRIPPED");
          tft->setTextSize(1);
          tft->setCursor(6, 34); tft->print("Over-current detected.");
          // Additional hint: relay suspected at trip
          int8_t r = protector.ocpTripRelay();
          const char* rname = nullptr;
          switch (r) {
            case R_LEFT:   rname = "LEFT";   break;
            case R_RIGHT:  rname = "RIGHT";  break;
            case R_BRAKE:  rname = "BRAKE";  break;
            case R_TAIL:   rname = "TAIL";   break;
            case R_MARKER: rname = "MARKER"; break;
            case R_AUX:    rname = "AUX";    break;
            case R_ENABLE: rname = "12V ENABLE"; break;
            default:       rname = nullptr;   break;
          }
          if (rname) {
            // Render on two lines to avoid truncation
            tft->setCursor(6, 46);
            tft->print("Possible short circuit on");
            tft->setCursor(6, 58);
            tft->print(rname);
          } else {
            tft->setCursor(6, 46);
            tft->print("Cycle OUTPUT to OFF.");
          }
          // Footer instruction
          tft->fillRect(0, 108, 160, 20, ST77XX_BLACK);
          tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
          tft->setCursor(6, 112); tft->print("Rotate to OFF to continue");
        }
        // Block until OFF is detected (debounced); keep relays off and extend suppression
        {
          uint32_t offStableStart = 0;
          while (true) {
            RotaryMode m = readRotary();
            for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
            if (m == MODE_ALL_OFF) {
              if (offStableStart == 0) offStableStart = millis();
              // Require OFF to be held stable for at least 300ms
              if (millis() - offStableStart >= 300) break;
            } else {
              offStableStart = 0; // reset stability if moved away
            }
            delay(10);
          }
        }
        // OFF detected: authorize and clear OCP latch; allow resume
        protector.setOcpClearAllowed(true);
        protector.clearOcpLatch();
        protector.setOcpHold(false);
        g_startupGuard = false; // guard will also clear in enforceRotaryMode when OFF seen
        tele.ocpLatched = false;
        ocpAcked = true;  // suppress further pop-ups until fault truly resolves
        // Ensure the Home screen fully repaints after leaving blocking modal
        ui->requestFullHomeRepaint();
        ui->showStatus(tele);
      }
    } else {
      // Not latched: start/continue healthy timer and re-arm after stable healthy window
      uint32_t now = millis();
      if (ocpHealthySince == 0) ocpHealthySince = now;
      if (now - ocpHealthySince >= 1000) {
        ocpAcked = false; // allow next trigger to show again
      }
      // OCP is not latched; ensure hold is released
      protector.setOcpHold(false);
    }
  }

  ui->setFaultMask(computeFaultMask());
  ui->tick(tele);

  // OUTV modal (single-shot per continuous fault; re-armed after healthy period)
  static bool     outvAcked = false;
  static uint32_t outvHealthySince = 0;
  {
    bool outvLatched = protector.isOutvLatched();
    if (outvLatched) {
      outvHealthySince = 0;
      if (!outvAcked) {
  (void)ui->protectionAlarm("OUTV LOW", "12V output low.", "Possible internal fault");
        // Allow user to attempt resume: clear OUTV latch on acknowledge (hard bounds still enforced in Protector)
        protector.clearOutvLatch();
        tele.outvLatched = false;
        outvAcked = true;
        // Ensure home fully repaints after leaving OUTV modal
        ui->requestFullHomeRepaint();
        ui->showStatus(tele);
      }
    } else {
      uint32_t now = millis();
      if (outvHealthySince == 0) outvHealthySince = now;
      if (now - outvHealthySince >= 1000) {
        outvAcked = false;
      }
    }
  }

  // LVP modal (single-shot; do NOT clear the latch on acknowledge so the home screen shows ACTIVE)
  static bool     lvpAcked = false;
  static uint32_t lvpHealthySince = 0;
  {
    bool lvpLatched = protector.isLvpLatched();
    if (lvpLatched) {
      lvpHealthySince = 0;
      if (!lvpAcked) {
        (void)ui->protectionAlarm("LVP TRIPPED", "Input voltage low.", "Press OK to continue");
        // Keep LVP latched so relays remain blocked and status shows ACTIVE
        lvpAcked = true;
        // Ensure home fully repaints after leaving LVP modal
        ui->requestFullHomeRepaint();
        ui->showStatus(tele);
      }
    } else {
      uint32_t now = millis();
      if (lvpHealthySince == 0) lvpHealthySince = now;
      if (now - lvpHealthySince >= 1000) {
        lvpAcked = false;
      }
    }
  }

  // If we booted a new OTA image in PENDING_VERIFY, mark it valid after a short stable run
  if (g_otaPendingVerify) {
    if (millis() - g_otaBootMs > 8000) { // ~8 seconds of healthy loop
      esp_ota_mark_app_valid_cancel_rollback();
      g_otaPendingVerify = false;
    }
  }

  // Let RF run, but we will enforce rotary below unless in MODE_RF_ENABLE
  RF::service();

  // Rotary has final say unless P2 (RF enabled)
  static RotaryMode s_prevMode = readRotary();
  RotaryMode curMode = readRotary();
  if (curMode != s_prevMode) {
    // On any mode change, suppress OCP for a short window to avoid false trips during relay transitions
    protector.suppressOcpUntil(millis() + 700); // tune 500–800ms as needed
    s_prevMode = curMode;
    // Only update stable mode for actual position changes, not fallback between-detent reads
    if (curMode != MODE_ALL_OFF || s_prevMode == MODE_ALL_OFF) {
      g_stableRotaryMode = curMode;
    }
  }
  enforceRotaryMode(curMode);

  delay(1); // keep UI responsive
}
