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

  // Update OCP limit at runtime (clamped to safety range)
  void setOcpLimit(float amps);

private:
  void tripLvp();
  void tripOcp();

  Preferences* _prefs = nullptr;
  float _lvp = 15.5f;   // volts
  float _ocp = 20.0f;   // amps

  // Limits for configuration
  static constexpr float OCP_MIN_A = 5.0f;
  static constexpr float OCP_MAX_A = 30.0f;

  // debounce / timing
  uint32_t _belowStartMs = 0;
  uint32_t _overStartMs  = 0;
  const uint32_t _lvpTripMs = 200;   // V below threshold for 200ms
  const uint32_t _ocpTripMs = 25;    // I above limit for 25ms

  // Auto-clear LVP when voltage recovers above threshold with hysteresis
  uint32_t _aboveClearStartMs = 0;         // begin time of healthy-above-LVP window
  const uint32_t _lvpClearMs   = 800;      // require 0.8s healthy before clearing latch
  const float    _lvpClearHyst = 0.3f;     // volts above cutoff required to clear

  // latches
  bool  _lvpLatched = false;
  bool  _ocpLatched = false;

  // LVP bypass: when true, LVP never trips (and existing LVP latch is cleared)
  bool  _lvpBypass = false;

  // ensure we cut once per boot even if relays were already off
  bool _cutsent = false;
};

extern Protector protector;
