#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Simple LVP/OCP protector. Debounced, latched trips; relay cut on trip.
// LVP can be bypassed via setLvpBypass(true).
class Protector {
public:
  void begin(Preferences* prefs, float lvpDefault = 15.5f, float ocpDefault = 20.0f);
  void tick(float srcV, float loadA, uint32_t nowMs);

  bool isLvpLatched() const { return _lvpLatched; }
  bool isOcpLatched() const { return _ocpLatched; }
  void clearLatches();      // clears both latches

  // LVP bypass control
  void setLvpBypass(bool on);
  bool lvpBypass() const { return _lvpBypass; }

  float lvp() const { return _lvp; }
  float ocp() const { return _ocp; }

private:
  void tripLvp();
  void tripOcp();

  Preferences* _prefs = nullptr;
  float _lvp = 15.5f;   // volts
  float _ocp = 20.0f;   // amps

  // debounce / timing
  uint32_t _belowStartMs = 0;
  uint32_t _overStartMs  = 0;
  const uint32_t _lvpTripMs = 200;   // V below threshold for 200ms
  const uint32_t _ocpTripMs = 25;    // I above limit for 25ms

  // latches
  bool  _lvpLatched = false;
  bool  _ocpLatched = false;

  // LVP bypass: when true, LVP never trips (and existing LVP latch is cleared)
  bool  _lvpBypass = false;

  // ensure we cut once per boot even if relays were already off
  bool _cutsent = false;
};

extern Protector protector;
