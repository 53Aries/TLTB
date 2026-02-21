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
  
  // ALERT pin configuration for extreme overcurrent (short circuit detection)
  // Configures ALERT pin to trigger when current exceeds threshold (default 30A)
  // Must be called after begin() and before attaching ISR
  void   configureAlert(float thresholdA = 30.0f);
  
  // Check if ALERT is currently active (pin is active-low)
  bool   isAlertActive();
  
  // Clear the alert condition by reading the Mask/Enable register
  void   clearAlert();
}

namespace INA226_SRC {
  extern bool PRESENT;

  void   begin();
  void   setCalibration(float rShuntOhms, float currentLsbA);
  float  readBusV();
  float  readCurrentA();
  
  // Relay coil monitoring (typical automotive relay coil: 50-150mA)
  // Returns total coil current through the source INA shunt
  float  getRelayCoilCurrent();
  
  // Check if measured coil current matches expected state
  // expectedCount: number of relays that should be ON
  // nominalCoilMa: expected current per relay coil (default 80mA)
  // Returns true if current is within expected range (±40% tolerance)
  bool   verifyRelayCoils(int expectedCount, float nominalCoilMa = 80.0f);
}
