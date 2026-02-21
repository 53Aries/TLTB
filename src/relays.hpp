// File Overview: Defines relay indices plus helper routines for safely driving the
// active-low outputs (including the master enable relay) on the tester.
#pragma once
#include <Arduino.h>
#include "pins.hpp"

// ----- Relay index map -----
enum RelayIndex : uint8_t {
  R_LEFT = 0,
  R_RIGHT,
  R_BRAKE,
  R_TAIL,
  R_MARKER,
  R_AUX,
  R_COUNT
};

// pins.hpp must provide RELAY_PIN[] with R_COUNT entries
static_assert(sizeof(RELAY_PIN) / sizeof(RELAY_PIN[0]) == R_COUNT,
              "RELAY_PIN[] size must equal R_COUNT");

// We track software state since OFF uses high-Z (INPUT)
// Single shared definition provided in relays.cpp
extern bool g_relay_on[R_COUNT];

// ----- Open-drain emulation on 3.3V MCU driving 5V LOW-trigger inputs -----
// ON  = sink to GND  -> pinMode(OUTPUT); digitalWrite(LOW)
// OFF = high-impedance -> pinMode(INPUT) (no pullup)

inline void _sinkOn(int pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);        // actively pull low
}

inline void _floatOff(int pin) {
  pinMode(pin, INPUT);           // high-Z; board's pull-up drives it HIGH (OFF)
  // (do NOT enable INPUT_PULLUP here)
}

// Core controls
inline void relayOn(RelayIndex r)  { _sinkOn(RELAY_PIN[(int)r]); g_relay_on[(int)r] = true; }
inline void relayOff(RelayIndex r) { _floatOff(RELAY_PIN[(int)r]); g_relay_on[(int)r] = false; }
inline bool relayIsOn(RelayIndex r){ return g_relay_on[(int)r]; }

// int overloads for convenience
inline void relayOn(int r)         { relayOn((RelayIndex)r); }
inline void relayOff(int r)        { relayOff((RelayIndex)r); }
inline bool relayIsOn(int r)       { return relayIsOn((RelayIndex)r); }

inline void relayToggle(RelayIndex r){
  if (relayIsOn(r)) relayOff(r); else relayOn(r);
}

// All OFF at once
inline void allOff(){
  for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
}

// One-time setup: ensure every channel is OFF (floating)
inline void relaysBegin(){
  for (int i = 0; i < (int)R_COUNT; ++i){
    _floatOff(RELAY_PIN[i]);      // OFF = INPUT (high-Z)
    g_relay_on[i] = false;
  }
}

// Optional: stable display names for primary user relays
inline const char* relayName(RelayIndex r){
  switch(r){
    case R_LEFT:   return "LEFT";
    case R_RIGHT:  return "RIGHT";
    case R_BRAKE:  return "BRAKE";
    case R_TAIL:   return "TAIL";
    case R_MARKER: return "MARKER";
    case R_AUX:    return "AUX";
    default:       return "R?";
  }
}

// ----- Relay Health Monitoring -----
// Count how many relays are currently expected to be ON
inline int countActiveRelays(){
  int count = 0;
  for (int i = 0; i < (int)R_COUNT; ++i){
    if (g_relay_on[i]) count++;
  }
  return count;
}

// Get bitmask of currently active relays (bit 0 = R_LEFT, bit 1 = R_RIGHT, etc.)
inline uint8_t getActiveRelayMask(){
  uint8_t mask = 0;
  for (int i = 0; i < (int)R_COUNT; ++i){
    if (g_relay_on[i]) mask |= (1 << i);
  }
  return mask;
}
