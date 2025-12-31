// File Overview: Declares the INA226 helper namespaces for interacting with the load
// and source sensors (current, bus voltage, presence, and polarity controls).
#pragma once
#include <Arduino.h>

namespace INA226 {
  extern bool  PRESENT;
  extern float OCP_LIMIT_A;

  void   begin();
  void   setOcpLimit(float a);
  float  readBusV();
  float  readCurrentA();
  bool   ocpActive();

  // Optional polarity inversion for load current
  void   setInvert(bool on);
  bool   getInvert();
}

namespace INA226_SRC {
  extern bool PRESENT;

  void   begin();
  float  readBusV();
}
