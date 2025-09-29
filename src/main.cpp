#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

extern "C" {
  #include "esp_task_wdt.h"
}

#include <WiFi.h>

#include "pins.hpp"
#include "prefs.hpp"
#include "display/DisplayUI.hpp"
#include "sensors/INA226.hpp"
#include "rf/RF.hpp"
#include "relays.hpp"
#include <Preferences.h>
#include "power/Protector.hpp"

// ---------------- Globals ----------------
static Adafruit_ST7735* tft = nullptr;   // shared SPI
static DisplayUI* ui = nullptr;
Preferences prefs;
static Telemetry tele{};

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

// ----------- Relay Scan -----------
static void scanAllOutputs(){
  if (ui) ui->showScanBegin();
  for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
  delay(30);
  for (int i = 0; i < (int)R_COUNT; ++i) {
    relayOn(i);
    if (ui) ui->showScanResult(i, "ON");
    delay(120);
    relayOff(i);
    delay(50);
  }
  if (ui) ui->showScanDone();
}

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
  // Avoid watchdog bites during bring-up (optional but harmless for this UI workload)
  esp_task_wdt_deinit();

  // TFT & encoder/buttons pins
  pinMode(PIN_TFT_BL, OUTPUT); digitalWrite(PIN_TFT_BL, HIGH);
  pinMode(PIN_TFT_CS, OUTPUT);  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_RST, OUTPUT);

  pinMode(PIN_ENC_A,    INPUT_PULLUP);
  pinMode(PIN_ENC_B,    INPUT_PULLUP);
  pinMode(PIN_ENC_OK,   INPUT_PULLUP);
  pinMode(PIN_ENC_BACK, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), enc_isrA, RISING);

  // Relays safe init
  relaysBegin();

  // SPI once (shared TFT + RF)
  SPI.begin(PIN_FSPI_SCK, PIN_FSPI_MISO, PIN_FSPI_MOSI, PIN_TFT_CS);

  // TFT reset + init
  digitalWrite(PIN_TFT_RST, HIGH); delay(20);
  digitalWrite(PIN_TFT_RST, LOW ); delay(50);
  digitalWrite(PIN_TFT_RST, HIGH); delay(120);

  tft = new Adafruit_ST7735(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
  tft->setSPISpeed(16000000UL);
  tft->initR(INITR_BLACKTAB);
  tft->setRotation(1);
  tft->fillScreen(ST77XX_BLACK);

  // Backlight (8-bit)
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(PIN_TFT_BL, BL_CHANNEL);
  ledcWrite(BL_CHANNEL, 255);

  // prefs first
  prefs.begin(NVS_NS, false);

  // UI wire-up
  ui = new DisplayUI(DisplayCtor{
    .pins        = {PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BL},
    .ns          = NVS_NS,
    .kBright     = KEY_BRIGHT,
    .kLvCut      = KEY_LV_CUTOFF,
    .kWifiSsid   = KEY_WIFI_SSID,
    .kWifiPass   = KEY_WIFI_PASS,
    .readSrcV    = [](){ return INA226_SRC::readBusV(); },
    .readLoadA   = [](){ return INA226::readCurrentA(); },
    .scanAll     = scanAllOutputs,
    .onOtaStart  = nullptr,
    .onOtaEnd    = nullptr,
    .onLvCutChanged = nullptr,
    .onOcpChanged   = nullptr,
    .onRfLearn      = RF::learn,
    .getLvpBypass   = [](){ return protector.lvpBypass(); },
    .setLvpBypass   = [](bool on){ protector.setLvpBypass(on); },
  });
  ui->attachTFT(tft, PIN_TFT_BL);
  ui->attachBrightnessSetter(setBacklight);
  ui->setEncoderReaders(readEncoderStep, okPressedEdge, backPressed);

  ui->begin(prefs);               // shows splash, applies brightness

  // sensors + RF (safe if missing)
  INA226::begin();
  INA226_SRC::begin();
  RF::begin();

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
  protector.begin(&prefs);

  ui->setFaultMask(computeFaultMask());
  ui->showStatus(tele);
}

void loop() {
  // Read telemetry if present
  tele.srcV  = INA226_SRC::PRESENT ? INA226_SRC::readBusV()    : NAN;
  tele.loadA = INA226::PRESENT     ? INA226::readCurrentA()    : NAN;

  // Protection logic (handles NANs and LVP bypass internally)
  protector.tick(tele.srcV, tele.loadA, millis());
  tele.lvpLatched = protector.isLvpLatched() || protector.isOcpLatched();

  // OCP modal
  static bool prevOcp = false;
  static bool needOcpAck = false;
  bool ocpNow = protector.isOcpLatched();
  if (ocpNow && !prevOcp) needOcpAck = true;
  prevOcp = ocpNow;

  if (needOcpAck) {
    bool clear = ui->protectionAlarm("OCP TRIPPED", "Over-current detected.", "Press OK to clear latch");
    if (clear) {
      protector.clearLatches();
      tele.lvpLatched = false;
    }
    needOcpAck = false;
    ui->showStatus(tele);
  }

  ui->setFaultMask(computeFaultMask());
  ui->tick(tele);
  RF::service();

  delay(1); // keep UI responsive
}
