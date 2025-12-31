// File Overview: Simple struct bundling the live voltage/current measurements and latch
// states that flow between the sensor, protection, and UI layers.
#pragma once
struct Telemetry {
  float srcV = 0.0f;
  float loadA = 0.0f;
  float outV = 0.0f;       // 12V buck output voltage (from LOAD INA226 bus voltage)
  bool  lvpLatched = false;
  bool  ocpLatched = false;
  bool  outvLatched = false; // Output Voltage Low/Fault latched
};
