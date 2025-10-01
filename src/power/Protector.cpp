#include "Protector.hpp"
#include <math.h>
#include "../prefs.hpp"
#include "../relays.hpp"

// Global instance
Protector protector;

void Protector::begin(Preferences* prefs, float lvpDefault, float ocpDefault) {
  _prefs = prefs;
  if (_prefs) {
    _lvp = _prefs->getFloat(KEY_LV_CUTOFF, lvpDefault);
    _ocp = _prefs->getFloat(KEY_OCP,      ocpDefault);
  } else {
    _lvp = lvpDefault;
    _ocp = ocpDefault;
  }
  _lvpLatched = _ocpLatched = false;
  _belowStartMs = _overStartMs = 0;
  _cutsent = false;
  _lvpBypass = false;  // not persisted (intentional: safe default on power-up)
}

void Protector::setLvpBypass(bool on) {
  _lvpBypass = on;
  if (on) {
    // clear any existing LVP latch when bypassing
    _lvpLatched = false;
  }
}

void Protector::tripLvp() {
  if (_lvpLatched) return;
  _lvpLatched = true;
  // immediate hard cut
  for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
  _cutsent = true;
}

void Protector::tripOcp() {
  if (_ocpLatched) return;
  _ocpLatched = true;
  // immediate hard cut
  for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
  _cutsent = true;
}

void Protector::clearLatches() {
  _lvpLatched = _ocpLatched = false;
  _belowStartMs = _overStartMs = 0;
  _cutsent = false;
}

void Protector::tick(float srcV, float loadA, uint32_t nowMs) {
  const bool haveV = !isnan(srcV);
  const bool haveI = !isnan(loadA);

  // -------- LVP (debounced), ignored if bypass enabled --------
  if (!_lvpBypass && haveV && srcV < _lvp) {
    if (_belowStartMs == 0) _belowStartMs = nowMs;
    if (!_lvpLatched && (nowMs - _belowStartMs) >= _lvpTripMs) tripLvp();
  } else {
    _belowStartMs = 0; // reset debounce if above threshold / missing / bypassing
  }

  // -------- OCP (debounced) --------
  if (haveI && loadA > _ocp) {
    if (_overStartMs == 0) _overStartMs = nowMs;
    if (!_ocpLatched && (nowMs - _overStartMs) >= _ocpTripMs) tripOcp();
  } else {
    _overStartMs = 0;
  }

  // -------- Continuous enforcement while latched --------
  // Previously this only cut once (gated by _cutsent). That allowed relays to be re-enabled later.
  // Now, while *either* latch is active, we force all relays OFF on every tick.
  if (_lvpLatched || _ocpLatched) {
    for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
    _cutsent = true;       // keep flag for backward compatibility
  } else {
    _cutsent = false;      // reset when no latches are active
  }
}
