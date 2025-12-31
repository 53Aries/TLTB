// File Overview: Implements the non-blocking buzzer state machine for confirmation beeps
// and repeating fault alarms tied to protection latches.
#include "buzzer.hpp"

namespace {
  enum class Mode : uint8_t { Idle, OneShot, Fault };  
  Mode g_mode = Mode::Idle;
  uint32_t g_until = 0;          // end time for current ON segment (for OneShot or Fault ON)
  uint32_t g_nextToggle = 0;      // next scheduled toggle time in fault pattern
  bool g_on = false;              // current pin state (true = buzzing)
  uint16_t g_oneshotLen = 60;     // stored requested oneshot length

  // Fault pattern constants
  constexpr uint16_t FAULT_ON_MS  = 200;
  constexpr uint16_t FAULT_OFF_MS = 800;

  inline void setOn(bool on){
    if (on == g_on) return;
    g_on = on;
    if (on) {
      pinMode(PIN_BUZZER, OUTPUT);
      digitalWrite(PIN_BUZZER, HIGH); // Active buzzer: drive HIGH to sound
    } else {
      // High-Z off (or drive LOW if module requires). We'll float to reduce idle draw.
      pinMode(PIN_BUZZER, INPUT);
    }
  }
}

namespace Buzzer {

void begin(){
  setOn(false);
  g_mode = Mode::Idle;
  g_until = g_nextToggle = 0;
}

void beep(uint16_t ms){
  // If currently in fault mode, ignore transient beep
  if (g_mode == Mode::Fault) return;
  // Start/restart one-shot
  g_mode = Mode::OneShot;
  g_oneshotLen = ms ? ms : 60;
  uint32_t now = millis();
  setOn(true);
  g_until = now + g_oneshotLen;
}

void tick(bool faultActive, uint32_t nowMs){
  // Fault state takes priority over any existing one-shot
  if (faultActive) {
    if (g_mode != Mode::Fault) {
      g_mode = Mode::Fault;
      setOn(true);
      g_nextToggle = nowMs + FAULT_ON_MS; // schedule first off
    }
  } else if (g_mode == Mode::Fault) {
    // Fault cleared: return to idle
    g_mode = Mode::Idle;
    setOn(false);
  }

  switch (g_mode) {
    case Mode::Idle:
      // nothing
      break;
    case Mode::OneShot:
      if ((int32_t)(nowMs - g_until) >= 0) {
        setOn(false);
        g_mode = Mode::Idle;
      }
      break;
    case Mode::Fault:
      if ((int32_t)(nowMs - g_nextToggle) >= 0) {
        if (g_on) {
          // Turn off and schedule next on
            setOn(false);
            g_nextToggle = nowMs + FAULT_OFF_MS;
        } else {
          setOn(true);
          g_nextToggle = nowMs + FAULT_ON_MS;
        }
      }
      break;
  }
}

} // namespace Buzzer
