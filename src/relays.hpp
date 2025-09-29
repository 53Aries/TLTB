// src/relays.hpp
#pragma once
#include <Arduino.h>
#include "pins.hpp"

// If pins.hpp doesn't define R_COUNT, derive it here.
#ifndef R_COUNT
  #define R_COUNT (int)(sizeof(RELAY_PIN)/sizeof(RELAY_PIN[0]))
#endif

inline void relaysBegin() {
  for (int i = 0; i < (int)R_COUNT; ++i) {
    pinMode(RELAY_PIN[i], OUTPUT);
    digitalWrite(RELAY_PIN[i], LOW);   // safe default
  }
}

inline void relayOn(int idx) {
  if (idx >= 0 && idx < (int)R_COUNT)
    digitalWrite(RELAY_PIN[idx], HIGH);
}

inline void relayOff(int idx) {
  if (idx >= 0 && idx < (int)R_COUNT)
    digitalWrite(RELAY_PIN[idx], LOW);
}
