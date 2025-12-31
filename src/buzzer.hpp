// File Overview: Declares the buzzer control API for short confirmation beeps and
// latched fault tone patterns on the active buzzer output.
#pragma once
#include <Arduino.h>
#include "pins.hpp"

// Simple non-blocking buzzer controller for an active 3V buzzer on PIN_BUZZER.
// Behavior:
//  - Single short beep (e.g. 60ms) on each RF-valid button press.
//  - Continuous pattern while any fault (OCP or LVP) latched: beep 200ms ON / 800ms OFF repeating.
//  - Fault pattern overrides transient RF beeps. When faults clear, pending RF beeps resume if within window.
namespace Buzzer {
  void begin();
  // Call each loop with current fault-latched state.
  void tick(bool faultActive, uint32_t nowMs);
  // Request a one-shot confirmation beep (ignored if a fault pattern is active).
  void beep(uint16_t ms = 60);
}
