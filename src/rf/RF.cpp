#include "RF.hpp"
#include <Arduino.h>
#include "pins.hpp"
#include "relays.hpp"
#include <Preferences.h>

#ifndef PIN_RF_DATA
#  error "Define PIN_RF_DATA in pins.hpp for SYN480R DATA input"
#endif

namespace {
  constexpr uint32_t QUIET_TIMEOUT_MS = 5000;
  constexpr uint32_t FRAME_GAP_US     = 2500;
  constexpr int      MAX_EDGES        = 120;
  constexpr uint32_t BIN_US           = 200;
  constexpr uint32_t MAX_PW_US        = 4000;

  volatile uint32_t g_edge_ts[MAX_EDGES];
  volatile int g_edge_cnt = 0;
  volatile uint32_t g_last_edge_us = 0;
  volatile bool g_frame_ready = false;

  uint32_t g_last_activity_ms = 0;

  struct Learned { uint32_t sig; uint8_t relay; };
  Learned g_learn[6];

  Preferences g_prefs;
  int8_t activeRelay = -1; // -1 = none on

  static inline uint32_t fnv1a32(uint32_t h, uint32_t v) {
    h ^= v; h *= 16777619u; return h;
  }

  void IRAM_ATTR isr_rf() {
    uint32_t now = micros();
    uint32_t dt = now - g_last_edge_us;
    g_last_edge_us = now;

    if (dt > FRAME_GAP_US) {
      if (g_edge_cnt > 8) {
        g_frame_ready = true;
      }
      g_edge_cnt = 0;
    }

    if (g_edge_cnt < MAX_EDGES) {
      g_edge_ts[g_edge_cnt++] = now;
    }
  }

  bool computeSignature(uint32_t &outHash) {
    noInterrupts();
    if (!g_frame_ready) { interrupts(); return false; }
    int n = g_edge_cnt;
    static uint32_t ts[MAX_EDGES];
    for (int i = 0; i < n; i++) ts[i] = g_edge_ts[i];
    g_frame_ready = false;
    interrupts();

    if (n < 10) return false;

    uint32_t h = 2166136261u;
    for (int i = 1; i < n; i++) {
      uint32_t pw = ts[i] - ts[i - 1];
      if (pw > MAX_PW_US) pw = MAX_PW_US;
      uint32_t q = pw / BIN_US;
      h = fnv1a32(h, q);
    }
    outHash = h;
    g_last_activity_ms = millis();
    return true;
  }

  void loadPrefs() {
    g_prefs.begin("tltb", false);
    for (int i = 0; i < 6; i++) {
      char key[16];
      snprintf(key, sizeof(key), "rf_sig%u", i);
      g_learn[i].sig = g_prefs.getULong(key, 0);
      snprintf(key, sizeof(key), "rf_rel%u", i);
      g_learn[i].relay = (uint8_t)g_prefs.getUChar(key, (uint8_t)i);
    }
  }

  void saveSlot(int i) {
    char key[16];
    snprintf(key, sizeof(key), "rf_sig%u", i);
    g_prefs.putULong(key, g_learn[i].sig);
    snprintf(key, sizeof(key), "rf_rel%u", i);
    g_prefs.putUChar(key, g_learn[i].relay);
  }

  void handleTrigger(uint8_t rindex) {
    if (rindex >= (uint8_t)R_COUNT) return;

    if (activeRelay == rindex) {
      // Same relay pressed again → turn it OFF
      relayOff((RelayIndex)rindex);
      activeRelay = -1;
      return;
    }

    // Turn off any other relay
    for (int i = 0; i < (int)R_COUNT; i++) {
      relayOff((RelayIndex)i);
    }

    // Turn this one ON
    relayOn((RelayIndex)rindex);
    activeRelay = rindex;
  }
} // namespace

bool RF::begin() {
  pinMode(PIN_RF_DATA, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_RF_DATA), isr_rf, CHANGE);
  loadPrefs();
  g_last_activity_ms = millis();
  return true;
}

void RF::service() {
  uint32_t sig;
  if (computeSignature(sig)) {
    for (int i = 0; i < 6; i++) {
      if (g_learn[i].sig != 0 && g_learn[i].sig == sig) {
        handleTrigger(g_learn[i].relay);
        return;
      }
    }
  }
}

bool RF::isPresent() {
  return (millis() - g_last_activity_ms) < QUIET_TIMEOUT_MS;
}

bool RF::learn(int relayIndex) {
  if (relayIndex < 0) relayIndex = 0;
  if (relayIndex > 5) relayIndex = 5;

  uint32_t start = millis();
  while (millis() - start < 5000) {
    uint32_t sig;
    if (computeSignature(sig)) {
      g_learn[relayIndex].sig = sig;
      g_learn[relayIndex].relay = (uint8_t)relayIndex;
      saveSlot(relayIndex);
      return true;
    }
    delay(5);
  }
  return false;
}
