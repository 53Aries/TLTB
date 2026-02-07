// File Overview: Application entry point for the Trailer Lighting Test Box; initializes
// hardware, drives the UI, samples telemetry, and coordinates protection plus relay logic.
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_ST7735.h>

// ESP-IDF C headers already provide their own extern "C" guards; direct includes keep this cleaner.
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "soc/rtc_cntl_reg.h"  // For brownout detector control

#include <WiFi.h>
#include "esp_coexist.h"

#include "pins.hpp"
#include "prefs.hpp"
#include "display/DisplayUI.hpp"
#include "sensors/INA226.hpp"
#include "rf/RF.hpp"
#include "buzzer.hpp"
#include "relays.hpp"
#include <Preferences.h>
#include "power/Protector.hpp"
#include "ble/TltbBleService.hpp"

// ---------------- Globals ----------------
static Adafruit_ST7735* tft = nullptr;   // shared SPI
static DisplayUI* ui = nullptr;
Preferences prefs;
static Telemetry tele{};
static bool g_startupGuard = false; // Prevents relay activation until 1p8t is cycled to OFF
static TltbBleService g_bleService;
static uint32_t g_faultMask = 0;
static constexpr bool kBypassInaPresenceCheck = true; // Temporary bypass when sensors are disconnected

// Cooldown timer state (20.5A usage limit)
static uint32_t g_highCurrentStartMs = 0; // When >20.5A current started (0=not active)
static uint32_t g_cooldownStartMs = 0;    // When cooldown period started (0=not in cooldown)
static constexpr uint32_t HIGH_CURRENT_LIMIT_MS = 120000; // 120 seconds
static constexpr uint32_t COOLDOWN_PERIOD_MS = 120000;     // 120 seconds
static constexpr float HIGH_CURRENT_THRESHOLD = 20.5f;     // 20.5 amps

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

  // Protection fault override: if any fault is latched, keep all relays OFF regardless of rotary position
  if (protector.isLvpLatched() || protector.isOcpLatched() || protector.isOutvLatched()) {
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
      #ifndef DEV_MODE
      allOff();
      #endif
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
        relayOn(R_AUX);
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

  if (!RF::isPresent())     m |= FLT_RF_MISSING;
  return m;
}

static bool bleCanDriveRelays() {
  if (g_startupGuard) return false;
  // Allow BLE control in RF mode OR in dev mode (bare ESP32 without rotary hardware)
  #ifdef DEV_MODE
  if (g_stableRotaryMode != MODE_RF_ENABLE && g_stableRotaryMode != MODE_ALL_OFF) {
    return false;
  }
  #else
  if (g_stableRotaryMode != MODE_RF_ENABLE) return false;
  #endif
  if (protector.isLvpLatched() || protector.isOcpLatched() || protector.isOutvLatched()) return false;
  return true;
}

static void handleBleRelayCommand(RelayIndex idx, bool desiredOn) {
  Serial.printf("[BLE] Relay command received: idx=%d, desiredOn=%d\n", (int)idx, desiredOn);
  if (!bleCanDriveRelays()) {
    Serial.printf("[BLE] Relay control blocked - startupGuard=%d, rotaryMode=%d (need %d for RF), lvp=%d, ocp=%d, outv=%d\n",
      g_startupGuard, (int)g_stableRotaryMode, (int)MODE_RF_ENABLE,
      protector.isLvpLatched(), protector.isOcpLatched(), protector.isOutvLatched());
    return;
  }
  int target = static_cast<int>(idx);
  if (target < (int)R_LEFT || target >= (int)R_ENABLE) return;
  if (desiredOn) {
    Serial.printf("[BLE] Turning relay %d ON\n", (int)idx);
    relayOn(idx);
  } else {
    Serial.printf("[BLE] Turning relay %d OFF\n", (int)idx);
    relayOff(idx);
  }
}

static const char* describeActiveLabel(RotaryMode mode) {
  if (g_startupGuard) {
    return "SAFE";
  }

  switch (mode) {
    case MODE_LEFT:   return "LEFT";
    case MODE_RIGHT:  return "RIGHT";
    case MODE_BRAKE:  return "BRAKE";
    case MODE_TAIL:   return "TAIL";
    case MODE_MARKER: return (getUiMode() == 1) ? "REV" : "MARK";
    case MODE_AUX:    return (getUiMode() == 1) ? "Ele Brakes" : "AUX";
    case MODE_RF_ENABLE: {
      int8_t rfRelay = RF::getActiveRelay();
      if (rfRelay >= (int)R_LEFT && rfRelay < (int)R_ENABLE) {
        return relayName(static_cast<RelayIndex>(rfRelay));
      }
      return "RF";
    }
    case MODE_ALL_OFF:
    default:
      return "OFF";
  }
}

// ---------------- setup/loop ----------------
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 1500) {
    delay(10);
  }
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("TLTB-BLE", ESP_LOG_DEBUG);

  esp_task_wdt_deinit();
  
  // Enable brownout detector to prevent running with unstable voltage
  // This prevents flash corruption during cold boots with slow voltage ramp
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1);

  // TFT & encoder/buttons pins
  // Keep backlight OFF until panel is fully initialized to avoid white-screen on cold power
  pinMode(PIN_TFT_BL, OUTPUT); digitalWrite(PIN_TFT_BL, LOW);
  pinMode(PIN_TFT_CS, OUTPUT);  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_RST, OUTPUT);
  
  // CRITICAL: Allow power rails to settle on cold battery connect
  // After hours unpowered, capacitors are fully discharged
  // Flash memory needs stable voltage before any read/write operations
  // 200ms gives adequate time for voltage regulators to stabilize
  delay(200);

  // Check if this app is booting in pending-verify state (OTA rollback flow)
  // NOTE: Rollback validation disabled - using simple OTA without state tracking
  // to avoid OTA data partition corruption issues
  {
    // Just log partition info for debugging
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
      // Running from partition: running->label
    }
  }
  pinMode(PIN_ENC_A,    INPUT_PULLUP);
  pinMode(PIN_ENC_B,    INPUT_PULLUP);
  pinMode(PIN_ENC_OK,   INPUT_PULLUP); // OK: idle HIGH (~3V3), pressed LOW
  pinMode(PIN_ENC_BACK, INPUT_PULLUP);

  // Factory recovery boot detection: BACK button held during power-on for 5 seconds
  // Boots to factory partition for OTA recovery when main partitions are corrupted
  delay(5);
  if (digitalRead(PIN_ENC_BACK) == LOW) {
    uint32_t holdStart = millis();
    
    // Wait for release or 5 second timeout
    while (digitalRead(PIN_ENC_BACK) == LOW) {
      if (millis() - holdStart >= 5000) {
        // Recovery mode: boot to factory partition
        // This allows OTA updates even if both OTA partitions are corrupted
        const esp_partition_t* factory = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
        
        if (factory) {
          Serial.println("\n=== FACTORY RECOVERY MODE ===");
          Serial.printf("Booting to factory partition at 0x%x\n", factory->address);
          
          esp_err_t err = esp_ota_set_boot_partition(factory);
          if (err == ESP_OK) {
            delay(100); // Allow serial to flush
            ESP.restart();
          } else {
            Serial.printf("Failed to set factory partition: %d\n", err);
          }
        } else {
          Serial.println("Factory partition not found - please flash factory firmware");
        }
        
        // If we get here, recovery failed - continue normal boot
        break;
      }
      delay(10);
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
    .onBleStop      = [](){ g_bleService.stopAdvertising(); },
    .onBleRestart   = [](){ g_bleService.restartAdvertising(); },
  });
  ui->attachTFT(tft, PIN_TFT_BL);
  ui->attachBrightnessSetter(setBacklight);
  ui->setEncoderReaders(readEncoderStep, okPressedEdge, backPressed);

  ui->begin(prefs);               // shows splash, applies brightness

  // Check for buck shutdown event (extreme current detected before power loss)
  {
    float extremeI = prefs.getFloat(KEY_EXTREME_I, 0.0f);
    if (extremeI >= 35.0f) {
      // Buck OCP likely caused shutdown - warn user
      tft->fillScreen(ST77XX_RED);
      tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
      tft->setTextSize(2);
      tft->setCursor(6, 6);
      tft->print("Overcurrent");
      tft->setTextSize(1);
      tft->setCursor(6, 34);
      tft->print("Extreme overcurrent");
      tft->setCursor(6, 46);
      tft->print("detected before restart.");
      tft->setCursor(6, 70);
      tft->printf("Current: %.1fA", extremeI);
      tft->setCursor(6, 82);
      tft->print("Possible short circuit.");
      // Footer instruction
      tft->fillRect(0, 108, 160, 20, ST77XX_BLACK);
      tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
      tft->setCursor(6, 112);
      tft->print("Check wiring & loads");
      delay(4000); // Give user time to read
      // Clear flag so we don't show on every boot
      prefs.remove(KEY_EXTREME_I);
      tft->fillScreen(ST77XX_BLACK);
    }
  }

  // Initialize sensors, RF, and buzzer
  INA226::begin();
  INA226_SRC::begin();
  RF::begin();
  Buzzer::begin();

  // Auto-join Wi-Fi (non-blocking)
  // NOTE: WiFi initialization moved to AFTER BLE init for better coexistence
  // BLE should start first, then WiFi joins network
  {
    // WiFi initialization deferred - see after BLE init below
  }

  // Protector init (loads thresholds)
  protector.begin(&prefs);
  ui->setFaultMask(computeFaultMask());
  // Don't show home screen yet - let battery detection run first with splash visible
  
  const bool sensorsMissing = (!INA226::PRESENT || !INA226_SRC::PRESENT);
  if (sensorsMissing) {
    if (!kBypassInaPresenceCheck) {
        tft->fillScreen(ST77XX_BLACK);
        tft->setTextColor(ST77XX_RED);
        tft->setTextSize(2);
        tft->setCursor(6, 6);
        tft->println("System Error");
        tft->setTextSize(1);
        tft->setTextColor(ST77XX_WHITE);
        tft->setCursor(6, 34);
        tft->println("Internal fault detected.");
        tft->setCursor(6, 46);
        tft->println("Device disabled.");
        tft->setCursor(6, 58);
        if (!INA226::PRESENT) tft->println("Load sensor missing.");
        if (!INA226_SRC::PRESENT) tft->println("Source sensor missing.");
        tft->setCursor(6, 82);
        tft->println("Contact support.");

        while (true) {
          for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
          delay(100);
        }
      } else {
        Serial.println("[APP] INA226 hardware missing; bypassing presence guard");
    }
  }
  
  // Boot-time off-current safety check: wait 1s for system to stabilize, then verify no unexpected load
  delay(1000);
  
  // Auto-detect battery type and set LVP (wait additional 2s for voltage to stabilize)
  delay(2000);
  ui->detectAndSetBatteryType();
  
  // Now show home screen after detection completes
  ui->showStatus(tele);
  
  // Read current after stabilization
  float bootCurrent = INA226::PRESENT ? INA226::readCurrentA() : 0.0f;
  if (!isnan(bootCurrent) && bootCurrent > 2.0f) {
    // Unexpected current draw at boot - critical safety issue (internal short or fault)
    tft->fillScreen(ST77XX_BLACK);
    tft->setTextColor(ST77XX_RED);
    tft->setTextSize(2);
    tft->setCursor(6, 6);
    tft->println("System Error");
    tft->setTextSize(1);
    tft->setTextColor(ST77XX_WHITE);
    tft->setCursor(6, 34);
    tft->println("Internal fault detected.");
    tft->setCursor(6, 46);
    tft->println("Unexpected load current.");
    tft->setCursor(6, 70);
    tft->println("Remove power NOW!");
    tft->setCursor(6, 94);
    tft->printf("Boot current: %.1fA", bootCurrent);
    // Block here forever - require power cycle
    while(true) {
      for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
      delay(100);
    }
  }

  BleCallbacks bleCallbacks{};
  bleCallbacks.onRelayCommand = [](RelayIndex idx, bool desiredOn) {
    handleBleRelayCommand(idx, desiredOn);
  };
  bleCallbacks.onRefreshRequest = []() {
    g_bleService.requestImmediateStatus();
  };
  g_bleService.begin("TLTB Controller", bleCallbacks);
  Serial.println("[APP] BLE begin invoked");

  // Initialize WiFi AFTER BLE for better coexistence
  // Enable WiFi/BLE coexistence to prevent radio conflicts and reboots
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  delay(100); // Allow BLE to stabilize before WiFi starts
  
  {
    String ssid = prefs.getString(KEY_WIFI_SSID, "");
    if (ssid.length()) {
      String pass = prefs.getString(KEY_WIFI_PASS, "");
      Serial.println("[APP] Starting WiFi...");
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(true); // Required for BLE coexistence
      WiFi.begin(ssid.c_str(), pass.c_str());
      Serial.printf("[APP] WiFi connecting to: %s\n", ssid.c_str());
    }
  }
}

void loop() {
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
          tft->setCursor(6, 6);  tft->print("Overcurrent");
          tft->setTextSize(1);
          tft->setCursor(6, 34); tft->print("Overcurrent condition.");
          tft->setCursor(6, 46); tft->print("System disabled.");
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
            // Render fault relay hint
            tft->setCursor(6, 58);
            tft->print("Check: ");
            tft->print(rname);
          }
          // Footer instruction
          tft->fillRect(0, 108, 160, 20, ST77XX_BLACK);
          tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
          tft->setCursor(6, 112); tft->print("Rotate to OFF to restart");
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

  g_faultMask = computeFaultMask();
  ui->setFaultMask(g_faultMask);
  ui->tick(tele);

  // OUTV modal (single-shot per continuous fault; re-armed after healthy period)
  static bool     outvAcked = false;
  static uint32_t outvHealthySince = 0;
  {
    bool outvLatched = protector.isOutvLatched();
    if (outvLatched) {
      outvHealthySince = 0;
      if (!outvAcked) {
        // Show a blocking modal that requires OFF position to clear
        if (tft) {
          tft->fillScreen(ST77XX_RED);
          tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
          tft->setTextSize(2);
          tft->setCursor(6, 6);  tft->print("Output V");
          tft->setTextSize(1);
          tft->setCursor(6, 34); tft->print("Output voltage fault.");
          tft->setCursor(6, 46); tft->print("Check system voltage.");
          // Footer instruction
          tft->fillRect(0, 108, 160, 20, ST77XX_BLACK);
          tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
          tft->setCursor(6, 112); tft->print("Rotate to OFF to restart");
        }
        // Block until OFF is detected (debounced); keep relays off
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
        // OFF detected: clear OUTV latch; allow resume
        protector.clearOutvLatch();
        tele.outvLatched = false;
        outvAcked = true;  // suppress further pop-ups until fault truly resolves
        // Ensure the Home screen fully repaints after leaving blocking modal
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

  // LVP modal (single-shot per continuous fault; re-armed after healthy period)
  static bool     lvpAcked = false;
  static uint32_t lvpHealthySince = 0;
  {
    bool lvpLatched = protector.isLvpLatched();
    if (lvpLatched) {
      lvpHealthySince = 0;
      if (!lvpAcked) {
        // Show a blocking modal that requires OFF position to clear
        if (tft) {
          tft->fillScreen(ST77XX_RED);
          tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
          tft->setTextSize(2);
          tft->setCursor(6, 6);  tft->print("LVP Tripped");
          tft->setTextSize(1);
          tft->setCursor(6, 34); tft->print("Battery voltage low.");
          tft->setCursor(6, 46); tft->print("Charge battery.");
          // Footer instruction
          tft->fillRect(0, 108, 160, 20, ST77XX_BLACK);
          tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
          tft->setCursor(6, 112); tft->print("Rotate to OFF to restart");
        }
        // Block until OFF is detected (debounced); keep relays off
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
        // OFF detected: clear LVP latch; allow resume
        protector.clearLvpLatch();
        tele.lvpLatched = false;
        lvpAcked = true;  // suppress further pop-ups until fault truly resolves
        // Ensure the Home screen fully repaints after leaving blocking modal
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

  // OTA validation disabled - using simple OTA
  // (Rollback protection removed to fix OTA data partition corruption)

  // Let RF run, but we will enforce rotary below unless in MODE_RF_ENABLE
  RF::service();

  // Rotary has final say unless P2 (RF enabled)
  static RotaryMode s_prevMode = readRotary();
  RotaryMode curMode = readRotary();
  if (curMode != s_prevMode) {
    // On any mode change, suppress OCP for a short window to avoid false trips during relay transitions
    protector.suppressOcpUntil(millis() + 700); // tune 500–800ms as needed
    
    // Reset RF state when entering or exiting RF mode
    if (curMode == MODE_RF_ENABLE || s_prevMode == MODE_RF_ENABLE) {
      RF::reset();
    }
    
    s_prevMode = curMode;
    // Only update stable mode for actual position changes, not fallback between-detent reads
    if (curMode != MODE_ALL_OFF || s_prevMode == MODE_ALL_OFF) {
      g_stableRotaryMode = curMode;
    }
  }
  enforceRotaryMode(curMode);

  BleStatusContext bleCtx{};
  bleCtx.telemetry = tele;
  bleCtx.faultMask = g_faultMask;
  bleCtx.startupGuard = g_startupGuard;
  bleCtx.lvpBypass = protector.lvpBypass();
  bleCtx.outvBypass = protector.outvBypass();
  bleCtx.enableRelay = relayIsOn(R_ENABLE);
  bleCtx.activeLabel = describeActiveLabel(g_stableRotaryMode);
  bleCtx.timestampMs = millis();
  bleCtx.uiMode = getUiMode();
  for (int i = 0; i < (int)R_COUNT; ++i) {
    bleCtx.relayStates[i] = relayIsOn(static_cast<RelayIndex>(i));
  }
  g_bleService.publishStatus(bleCtx);

  delay(1); // keep UI responsive
}
