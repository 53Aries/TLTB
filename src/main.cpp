// =============================================================================
// TLTB - Trailer Lighting Test Box
// Main application entry point
// 
// Responsibilities:
//   - Hardware initialization (display, sensors, RF, BLE)
//   - UI rendering and user input processing
//   - Telemetry sampling and protection monitoring
//   - Relay control coordination
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_ST7735.h>

// ESP-IDF headers
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "soc/rtc_cntl_reg.h"

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

// =============================================================================
// Global State
// =============================================================================

static Adafruit_ST7735* tft = nullptr;    // Shared SPI display
static DisplayUI* ui = nullptr;            // UI controller
Preferences prefs;                         // NVS storage
static Telemetry tele{};                   // Telemetry data
static TltbBleService g_bleService;        // BLE service
static uint32_t g_faultMask = 0;           // Active fault flags

// Startup guard: prevents relay activation until 1p8t switch is cycled to OFF
static bool g_startupGuard = false;

// BLE relay tracking: which relay was last activated via BLE (-1 = none)
static int8_t g_bleActiveRelay = -1;

// Temporary bypass for INA226 presence check when sensors are disconnected
static constexpr bool kBypassInaPresenceCheck = true;

// =============================================================================
// High Current Monitoring & Cooldown
// Enforces 20.5A limit: 120 seconds on, 120 seconds cooldown
// =============================================================================

static uint32_t g_highCurrentStartMs = 0;  // Timestamp when >20.5A started (0 = inactive)
static uint32_t g_cooldownStartMs = 0;     // Timestamp when cooldown started (0 = not in cooldown)

static constexpr uint32_t HIGH_CURRENT_LIMIT_MS = 120000;  // Maximum high current duration (seconds)
static constexpr uint32_t COOLDOWN_PERIOD_MS = 120000;     // Required cooldown time (2 minutes)
static constexpr float HIGH_CURRENT_THRESHOLD = 20.5f;     // Current threshold (amps)

// =============================================================================
// INA226 ALERT Pin ISR (Short Circuit Detection)
// =============================================================================
// ALERT pin triggers on extreme overcurrent (>30A) before buck shuts down
// ISR writes flag to NVS immediately for detection on next boot
static volatile bool g_alertTriggered = false;

void IRAM_ATTR ina_alert_isr() {
  g_alertTriggered = true;
  // Flag will be written to NVS in main loop to avoid I2C in ISR
}

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
  if (protector.isLvpLatched() || protector.isOcpLatched() || protector.isOutvLatched() || protector.isRelayCoilLatched()) {
    for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
    return;
  }

  // Normal operation: In all non-RF modes, we *force* the relay states each loop.
  // This guarantees RF is effectively ignored unless in MODE_RF_ENABLE.
  auto allOff = [](){
    // Turn off all output relays
    for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
  };

  // Clear BLE active relay tracking when not in RF mode
  // (BLE can only control relays in RF mode, so this keeps display accurate)
  if (m != MODE_RF_ENABLE) {
    g_bleActiveRelay = -1;
  }

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
}

// (Relay scan feature removed)

// ----------- Faults -----------
static uint32_t computeFaultMask(){
  uint32_t m = FLT_NONE;
  if (!INA226::PRESENT)     m |= FLT_INA_LOAD_MISSING;
  if (!INA226_SRC::PRESENT) m |= FLT_INA_SRC_MISSING;

  if (!RF::isPresent())     m |= FLT_RF_MISSING;
  if (protector.isRelayCoilLatched()) m |= FLT_RELAY_COIL;
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
  if (protector.isLvpLatched() || protector.isOcpLatched() || protector.isOutvLatched() || protector.isRelayCoilLatched()) return false;
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
  if (target < (int)R_LEFT || target >= (int)R_COUNT) return;
  if (desiredOn) {
    Serial.printf("[BLE] Turning relay %d ON\n", (int)idx);
    relayOn(idx);
    g_bleActiveRelay = (int8_t)idx; // Track active BLE relay for display
  } else {
    Serial.printf("[BLE] Turning relay %d OFF\n", (int)idx);
    relayOff(idx);
    // Clear active relay if this was the active one
    if (g_bleActiveRelay == (int8_t)idx) {
      g_bleActiveRelay = -1;
    }
  }
}

static const char* describeActiveLabel(RotaryMode mode) {
  if (g_startupGuard) {
    return "SAFE";
  }

  // Check if BLE has an active relay - takes priority over rotary position
  // This allows the display to show what's actually on when controlled via app
  if (g_bleActiveRelay >= (int)R_LEFT && g_bleActiveRelay < (int)R_COUNT) {
    // Verify the relay is actually still on
    if (relayIsOn(g_bleActiveRelay)) {
      return relayName(static_cast<RelayIndex>(g_bleActiveRelay));
    } else {
      // Relay was turned off by other means, clear tracking
      g_bleActiveRelay = -1;
    }
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
      if (rfRelay >= (int)R_LEFT && rfRelay < (int)R_COUNT) {
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

  // TFT & encoder/buttons pins
  // TFT backlight not used - display runs without it (GPIO 42 repurposed for INA ALERT)
  pinMode(PIN_TFT_CS, OUTPUT);  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_RST, OUTPUT);

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

  // TFT backlight not used (GPIO 42 repurposed for INA ALERT pin)

  // prefs first
  prefs.begin(NVS_NS, false);

  // Dev-boot now uses existing UI Wi‑Fi/OTA pages; no special flow here.

  // UI wire-up
  ui = new DisplayUI(DisplayCtor{
    .pins        = {PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, -1},  // BL not used
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
    .onBleStop      = [](){ g_bleService.shutdownForOta(); },
    .onBleRestart   = [](){ g_bleService.restartAfterOta(); },
  });
  ui->attachTFT(tft, -1);  // Backlight not used
  // No brightness setter needed
  ui->setEncoderReaders(readEncoderStep, okPressedEdge, backPressed);

  ui->begin(prefs);               // shows splash, applies brightness

  // Check for buck shutdown event (short circuit or extreme current)
  {
    // Check ALERT-triggered short circuit
    bool shortCircuit = prefs.getBool(KEY_SHORT_CIRCUIT, false);
    float extremeI = prefs.getFloat(KEY_EXTREME_I, 0.0f);
    int8_t relayIdx = prefs.getChar(KEY_SHORT_RELAY, -1);
    
    if (shortCircuit || extremeI >= 35.0f) {
      // Buck OCP likely caused shutdown - warn user
      tft->fillScreen(ST77XX_RED);
      tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
      tft->setTextSize(2);
      tft->setCursor(6, 6);
      tft->print("SHORT CIRCUIT");
      tft->setTextSize(1);
      
      // Show which output caused the issue
      tft->setCursor(6, 34);
      tft->print("Extreme Overcurrent");
      tft->setCursor(6, 46);
      if (relayIdx >= 0 && relayIdx < (int)R_COUNT) {
        tft->printf("detected on %s", relayName((RelayIndex)relayIdx));
      } else {
        tft->print("detected");
      }
      
      tft->setCursor(6, 58);
      tft->print("Possible Short Circuit");
      
      tft->setCursor(6, 76);
      if (extremeI >= 30.0f) {
        tft->printf("Current: %.1fA", extremeI);
      }
      
      tft->setCursor(6, 88);
      tft->print("Check wiring & loads.");
      
      // Footer instruction
      tft->fillRect(0, 108, 160, 20, ST77XX_BLACK);
      tft->setTextColor(ST77XX_YELLOW, ST77XX_BLACK);
      tft->setCursor(6, 112);
      tft->print("Contact service if needed");
      delay(5000); // Give user time to read
      // Clear flags so we don't show on every boot
      prefs.remove(KEY_SHORT_CIRCUIT);
      prefs.remove(KEY_EXTREME_I);
      prefs.remove(KEY_SHORT_RELAY);
      tft->fillScreen(ST77XX_BLACK);
    }
  }

  // Initialize sensors, RF, and buzzer
  INA226::begin();
  INA226_SRC::begin();
  
  // Configure INA226 ALERT pin for extreme overcurrent detection (>30A)
  // ALERT triggers before buck converter shutdown, allowing ISR to log event
  if (INA226::PRESENT) {
    INA226::configureAlert(30.0f);  // Trigger at 30A
    pinMode(PIN_INA_LOAD_ALERT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_INA_LOAD_ALERT), ina_alert_isr, FALLING);
    Serial.println("[APP] INA226 ALERT ISR attached on GPIO 42");
  }
  
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

  // WiFi is NOT initialized at startup - it starts only when needed (OTA or WiFi scan)
  // This gives BLE full antenna access for maximum reliability
  // WiFi/BLE coexistence is configured when WiFi starts in Ota.cpp
  Serial.println("[APP] WiFi disabled - BLE has full antenna access");
  Serial.println("[APP] WiFi will start automatically when OTA update is triggered");
}

void loop() {
  // Check for INA226 ALERT trigger (short circuit detection)
  // Write to NVS immediately when alert fires (before buck shutdown)
  if (g_alertTriggered) {
    g_alertTriggered = false;
    float current = INA226::PRESENT ? INA226::readCurrentA() : 0.0f;
    
    // Capture which relay was active when short circuit occurred
    int8_t activeRelay = -1;
    for (int i = 0; i < (int)R_COUNT; ++i) {
      if (relayIsOn(i)) {
        activeRelay = (int8_t)i;
        break;  // Take first active relay
      }
    }
    
    prefs.putBool(KEY_SHORT_CIRCUIT, true);
    prefs.putFloat(KEY_EXTREME_I, current);
    prefs.putChar(KEY_SHORT_RELAY, activeRelay);
    Serial.printf("[ALERT] Short circuit detected! Current=%.1fA, relay=%d, flags written to NVS\n", current, activeRelay);
    INA226::clearAlert();  // Clear latch for next detection
  }
  
  // Read telemetry if present
  tele.srcV  = INA226_SRC::PRESENT ? INA226_SRC::readBusV()    : NAN;
  tele.loadA = INA226::PRESENT     ? INA226::readCurrentA()    : NAN;
  tele.outV  = INA226::PRESENT     ? INA226::readBusV()        : NAN; // LOAD INA226 bus voltage as buck output
  
  // Read relay coil current (SOURCE INA226 monitors coil current)
  tele.relayCoilA = INA226_SRC::PRESENT ? INA226_SRC::readCurrentA() : NAN;
  
  // Relay health check: verify coil current matches expected state (run every 500ms)
  static uint32_t lastRelayCheckMs = 0;
  uint32_t now = millis();
  if ((now - lastRelayCheckMs) >= 500) {
    lastRelayCheckMs = now;
    int expectedRelays = countActiveRelays();
    bool relayHealthOk = INA226_SRC::verifyRelayCoils(expectedRelays);
    
    // Trip relay coil fault if mismatch detected (not already latched)
    if (!relayHealthOk && !protector.isRelayCoilLatched()) {
      // Determine which relay (if any) is suspected to be faulty
      int faultyRelay = -1;
      // If only one relay should be on, that's the suspect
      if (expectedRelays == 1) {
        for (int i = 0; i < (int)R_COUNT; ++i) {
          if (relayIsOn(i)) { faultyRelay = i; break; }
        }
      }
      Serial.printf("[RELAY] Coil fault detected: expected %d relays (%.1fmA), measured %.1fmA\n",
                    expectedRelays, expectedRelays * 80.0f, tele.relayCoilA * 1000.0f);
      protector.tripRelayCoil(faultyRelay);
    }
  }

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
  tele.relayCoilLatched = protector.isRelayCoilLatched();

  // Cooldown timer logic: limit sustained high current usage
  float current = !isnan(tele.loadA) ? fabsf(tele.loadA) : 0.0f;
  
  if (g_cooldownStartMs > 0) {
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
    if (tele.relayCoilLatched) beepFault = true; // Relay coil fault always beeps
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
  
  // Update display with current active label (includes BLE tracking)
  ui->setActiveLabel(describeActiveLabel(g_stableRotaryMode));
  
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

  // Relay Coil Fault modal (single-shot per continuous fault; re-armed after healthy period)
  static bool     relayCoilAcked = false;
  static uint32_t relayCoilHealthySince = 0;
  {
    bool relayCoilLatched = protector.isRelayCoilLatched();
    if (relayCoilLatched) {
      g_startupGuard = true; // Prevent relay operation until OFF cycle
      relayCoilHealthySince = 0;
      if (!relayCoilAcked) {
        // Show a blocking modal that requires OFF position to clear
        if (tft) {
          tft->fillScreen(ST77XX_RED);
          tft->setTextColor(ST77XX_WHITE, ST77XX_RED);
          tft->setTextSize(2);
          tft->setCursor(6, 6);  tft->print("Relay Fault");
          tft->setTextSize(1);
          
          // Show which relay is suspected if known
          int8_t faultyIdx = protector.relayCoilFaultIndex();
          if (faultyIdx >= 0 && faultyIdx < (int)R_COUNT) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Output %s", relayName((RelayIndex)faultyIdx));
            tft->setCursor(6, 34); tft->print(buf);
            tft->setCursor(6, 46); tft->print("internal fault.");
          } else {
            tft->setCursor(6, 34); tft->print("Output internal fault.");
          }
          
          tft->setCursor(6, 58); tft->print("Contact customer svc.");
          
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
        // OFF detected: clear relay coil latch; allow resume
        protector.clearRelayCoilLatch();
        g_startupGuard = false;
        tele.relayCoilLatched = false;
        relayCoilAcked = true;  // suppress further pop-ups until fault truly resolves
        // Ensure the Home screen fully repaints after leaving blocking modal
        ui->requestFullHomeRepaint();
        ui->showStatus(tele);
      }
    } else {
      uint32_t now = millis();
      if (relayCoilHealthySince == 0) relayCoilHealthySince = now;
      if (now - relayCoilHealthySince >= 1000) {
        relayCoilAcked = false;
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
  bleCtx.activeLabel = describeActiveLabel(g_stableRotaryMode);
  bleCtx.timestampMs = millis();
  bleCtx.uiMode = getUiMode();
  for (int i = 0; i < (int)R_COUNT; ++i) {
    bleCtx.relayStates[i] = relayIsOn(static_cast<RelayIndex>(i));
  }
  g_bleService.publishStatus(bleCtx);

  delay(1); // keep UI responsive
}
