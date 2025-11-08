#include "Protector.hpp"
#include <math.h>
#include "prefs.hpp"
#include "relays.hpp"

// Global instance
Protector protector;

void Protector::begin(Preferences* prefs, float lvpDefault, float ocpDefault) {
  _prefs = prefs;
  if (_prefs) {
    _lvp = _prefs->getFloat(KEY_LV_CUTOFF, lvpDefault);
    _ocp = _prefs->getFloat(KEY_OCP,      ocpDefault);
    _outvCut = _prefs->getFloat(KEY_OUTV_CUTOFF, _outvCut);
  } else {
    _lvp = lvpDefault;
    _ocp = ocpDefault;
  }
  // Enforce OCP bounds on startup to protect against out-of-range saved values
  if (_ocp < OCP_MIN_A) _ocp = OCP_MIN_A;
  if (_ocp > OCP_MAX_A) _ocp = OCP_MAX_A;
  // Enforce OutV bounds (8..16V)
  if (_outvCut < OUTV_MIN_V) _outvCut = OUTV_MIN_V;
  if (_outvCut > OUTV_MAX_V) _outvCut = OUTV_MAX_V;
  _lvpLatched = _ocpLatched = false;
  _belowStartMs = _overStartMs = 0;
  _outvLatched = false;
  _outvBelowStartMs = 0;
  _cutsent = false;
  _lvpBypass = false;  // not persisted (intentional: safe default on power-up)
}

void Protector::setOcpLimit(float amps) {
  if (amps < OCP_MIN_A) amps = OCP_MIN_A;
  if (amps > OCP_MAX_A) amps = OCP_MAX_A;
  _ocp = amps;
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
  _lvpLatched = _ocpLatched = _outvLatched = false;
  _belowStartMs = _overStartMs = 0;
  _outvBelowStartMs = 0;
  _cutsent = false;
}

void Protector::setOutvCutoff(float v) {
  if (v < OUTV_MIN_V) v = OUTV_MIN_V;
  if (v > OUTV_MAX_V) v = OUTV_MAX_V;
  _outvCut = v;
}

void Protector::tick(float srcV, float loadA, float outV, uint32_t nowMs) {
  const bool haveV = !isnan(srcV);
  const bool haveI = !isnan(loadA);
  const bool haveOutV = !isnan(outV);

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

  // -------- Output Voltage Low (debounced around user cutoff) --------
  if (haveOutV) {
    // Hard instant trips for out-of-bounds extremes
    if (outV < OUTV_MIN_V || outV > OUTV_MAX_V) {
      if (!_outvLatched) {
        _outvLatched = true;
        for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
      }
    } else if (outV < _outvCut) {
      if (_outvBelowStartMs == 0) _outvBelowStartMs = nowMs;
      if (!_outvLatched && (nowMs - _outvBelowStartMs) >= _outvTripMs) {
        _outvLatched = true;
        for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
      }
    } else {
      _outvBelowStartMs = 0; // healthy again; no auto-clear (require manual)
    }
  }

  // -------- LVP auto-clear when voltage healthy for a while --------
  if (_lvpLatched) {
    // Require srcV to be sufficiently above cutoff (with hysteresis) for a period
    if (haveV && srcV >= (_lvp + _lvpClearHyst)) {
      if (_aboveClearStartMs == 0) _aboveClearStartMs = nowMs;
      if ((nowMs - _aboveClearStartMs) >= _lvpClearMs) {
        _lvpLatched = false;
        _aboveClearStartMs = 0;
      }
    } else {
      _aboveClearStartMs = 0; // lost healthy condition; restart timer
    }
  } else {
    _aboveClearStartMs = 0;   // not latched; keep clear window idle
  }

  // -------- Continuous enforcement while latched --------
  // Previously this only cut once (gated by _cutsent). That allowed relays to be re-enabled later.
  // Now, while *either* latch is active, we force all relays OFF on every tick.
  if (_lvpLatched || _ocpLatched || _outvLatched) {
    for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
    _cutsent = true;       // keep flag for backward compatibility
  } else {
    _cutsent = false;      // reset when no latches are active
  }
}
