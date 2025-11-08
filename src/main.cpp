#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
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
#include "ota/Ota.hpp"

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
  bool cur = (digitalRead(PIN_ENC_OK) == LOW);
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
    for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
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
      allOff(); relayOn(R_AUX);
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
  pinMode(PIN_ENC_OK,   INPUT_PULLUP);
  pinMode(PIN_ENC_BACK, INPUT_PULLUP);

  // Early dev-boot detection: rotary at position 8 and BACK held during power-on
  pinMode(PIN_ROT_P8, INPUT_PULLUP);
  delay(5);
  if (digitalRead(PIN_ENC_BACK) == LOW && digitalRead(PIN_ROT_P8) == LOW) {
    g_devBoot = true;
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
    .onLvCutChanged = nullptr,
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
    ui->setDevMenuOnly(true); // restrict menu to Wi‑Fi/OTA and start in menu
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
  }
}

void loop() {
  if (g_devBoot) {
    // Dev boot: only UI for Wi‑Fi and OTA
    ui->tick(tele);
    delay(1);
    return;
  }

  // Read telemetry if present
  tele.srcV  = INA226_SRC::PRESENT ? INA226_SRC::readBusV()    : NAN;
  tele.loadA = INA226::PRESENT     ? INA226::readCurrentA()    : NAN;
  tele.outV  = INA226::PRESENT     ? INA226::readBusV()        : NAN; // LOAD INA226 bus voltage as buck output

  // Protection logic
  protector.tick(tele.srcV, tele.loadA, tele.outV, millis());
  // Track latches separately for UI clarity
  tele.lvpLatched   = protector.isLvpLatched();
  tele.ocpLatched   = protector.isOcpLatched();
  tele.outvLatched  = protector.isOutvLatched();
  // Buzzer fault pattern tick (priority over one-shot)
  // Suppress buzzer for LVP/OUTV when those protections are bypassed; include off-current fault
  {
    bool beepFault = false;
    if (tele.ocpLatched) beepFault = true;
    if (tele.lvpLatched && !protector.lvpBypass()) beepFault = true;
    if (tele.outvLatched && !protector.outvBypass()) beepFault = true;
    // Off-current fault: all relays OFF and load current > 1.0A
    bool anyOn = false;
    for (int i = 0; i < (int)R_COUNT; ++i) { if (relayIsOn(i)) { anyOn = true; break; } }
    bool allOff = !anyOn;
    bool overCur = (!isnan(tele.loadA)) && (fabsf(tele.loadA) > 1.0f);
    if (allOff && overCur) beepFault = true;
    Buzzer::tick(beepFault, millis());
  }

  // OCP modal (single-shot per continuous fault; re-armed after healthy period)
  static bool     ocpAcked = false;           // has the current OCP fault cycle been acknowledged?
  static uint32_t ocpHealthySince = 0;        // ms timestamp when OCP became healthy (unlatched)
  {
    bool ocpLatched = protector.isOcpLatched();
    if (ocpLatched) {
      ocpHealthySince = 0; // fault persists; not healthy
      if (!ocpAcked) {
        (void)ui->protectionAlarm("OCP TRIPPED", "Over-current detected.", "Press OK to clear");
        // Allow user to attempt resume: clear the OCP latch on acknowledge
        protector.clearOcpLatch();
        tele.ocpLatched = false;
        ocpAcked = true;  // suppress further pop-ups until fault truly resolves
        ui->showStatus(tele);
      }
    } else {
      // Not latched: start/continue healthy timer and re-arm after stable healthy window
      uint32_t now = millis();
      if (ocpHealthySince == 0) ocpHealthySince = now;
      if (now - ocpHealthySince >= 1000) {
        ocpAcked = false; // allow next trigger to show again
      }
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

  // Off-current protection: if all relays are OFF (12V system disabled) but load current > 1.0A,
  // show a high-priority single-shot alert instructing to remove power.
  static bool     offCurAcked = false;
  static uint32_t offCurHealthySince = 0;
  {
    bool anyOn = false;
    for (int i = 0; i < (int)R_COUNT; ++i) { if (relayIsOn(i)) { anyOn = true; break; } }
    bool allOff = !anyOn;
    bool overCur = (!isnan(tele.loadA)) && (fabsf(tele.loadA) > 1.0f);
    bool offCurrentFault = allOff && overCur;

    if (offCurrentFault) {
      offCurHealthySince = 0;
      if (!offCurAcked) {
        (void)ui->protectionAlarm("CURRENT DRAW", "UNEXPECTED CURRENT DRAW", "REMOVE POWER NOW!");
        offCurAcked = true; // suppress repeated popups until healthy
        ui->showStatus(tele);
      }
    } else {
      uint32_t now = millis();
      if (offCurHealthySince == 0) offCurHealthySince = now;
      if (now - offCurHealthySince >= 1000) {
        offCurAcked = false;
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
  enforceRotaryMode(readRotary());

  delay(1); // keep UI responsive
}
