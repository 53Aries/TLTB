#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "../pins.hpp"

// Compile-time kill switch (optional). You can also pass -DRF_ENABLE=0 in platformio.ini
#ifndef RF_ENABLE
#define RF_ENABLE 1
#endif

namespace RF {
  void begin();      // safe if no module present
  void service();    // no-op for now
  bool isPresent();  // true iff CC1101 probed OK

  bool learn(int buttonIdx);            // stubs for now
  bool tx(int buttonIdx, bool onOff);

  void setFrequency(float mhz);         // default 433.92
  void setPA(int pa);                   // default 10
}
