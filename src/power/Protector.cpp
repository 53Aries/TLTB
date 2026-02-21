// File Overview: Implements the protection state machine handling LVP, OCP, and output
// voltage faults, including debounce logic, latching, bypassing, and relay cutoffs.
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
  // Clamp LVP to allowed range
  if (_lvp < LVP_MIN_V) _lvp = LVP_MIN_V;
  if (_lvp > LVP_MAX_V) _lvp = LVP_MAX_V;
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
  _relayCoilLatched = false;
  _relayCoilFaultIndex = -1;
  _cutsent = false;
  _lvpBypass = false;  // not persisted (intentional: safe default on power-up)
  _outvBypass = false;
  // Initialize OCP grace idle; will be armed on first over-current event
  _ocpGraceUntilMs = 0;
  _ocpTripRelay = -1;
}

void Protector::setOcpLimit(float amps) {
  if (amps < OCP_MIN_A) amps = OCP_MIN_A;
  if (amps > OCP_MAX_A) amps = OCP_MAX_A;
  _ocp = amps;
}

void Protector::setLvpCutoff(float v) {
  if (v < LVP_MIN_V) v = LVP_MIN_V;
  if (v > LVP_MAX_V) v = LVP_MAX_V;
  _lvp = v;
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
  // Capture which relay was ON at the moment of trip (before hard cut)
  _ocpTripRelay = -1;
  for (int i = 0; i < (int)R_COUNT; ++i) {
    if (relayIsOn(i)) { _ocpTripRelay = (int8_t)i; break; }
  }
  // immediate hard cut
  for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
  _cutsent = true;
}

void Protector::tripRelayCoil(int relayIndex) {
  if (_relayCoilLatched) return;
  _relayCoilLatched = true;
  _relayCoilFaultIndex = (int8_t)relayIndex;
  // immediate hard cut
  for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
  _cutsent = true;
}

void Protector::clearLatches() {
  _lvpLatched = _ocpLatched = _outvLatched = _relayCoilLatched = false;
  _belowStartMs = _overStartMs = 0;
  _outvBelowStartMs = 0;
  _cutsent = false;
}

void Protector::clearLvpLatch(){
  _lvpLatched = false;
  _belowStartMs = 0;
  _aboveClearStartMs = 0;
}

void Protector::clearOcpLatch(){
  if (!_ocpClearAllowed) return; // ignore clears unless explicitly allowed
  _ocpLatched = false;
  _overStartMs = 0;
  _ocpTripRelay = -1;
  _ocpClearAllowed = false; // consume permission
}

void Protector::clearOutvLatch(){
  _outvLatched = false;
  _outvBelowStartMs = 0;
}

void Protector::clearRelayCoilLatch(){
  _relayCoilLatched = false;
  _relayCoilFaultIndex = -1;
}

void Protector::setOutvBypass(bool on) {
  _outvBypass = on;
  if (on) {
    _outvLatched = false; // clear existing OUTV latch when bypassed
    _outvBelowStartMs = 0;
  }
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

  // -------- OCP with transient suppression + two-tier protection --------
  // Tier 1: Instant trip for extreme overcurrent (>2x OCP limit) - likely short circuit
  // Tier 2: Fast debounced trip for moderate overload (>OCP limit, <2x OCP limit)
  bool ocpSuppressed = (_ocpSuppressUntilMs != 0) && (nowMs < _ocpSuppressUntilMs);
  if (!ocpSuppressed && haveI && loadA > _ocp) {
    // Check for extreme overcurrent requiring instant trip
    float instantTripThreshold = _ocp * OCP_INSTANT_MULTIPLIER;
    if (loadA >= instantTripThreshold) {
      // INSTANT TRIP: No debounce for catastrophic overcurrent (likely short)
      if (!_ocpLatched) {
        tripOcp();
      }
    } else {
      // MODERATE OVERLOAD: Use reduced debounce (10ms instead of 25ms)
      if (_overStartMs == 0) _overStartMs = nowMs;
      const uint32_t fastTripMs = 10;  // Reduced from 25ms for faster moderate overload response
      if ((nowMs - _overStartMs) >= fastTripMs) {
        if (!_ocpLatched) {
          tripOcp();
        }
      }
    }
  } else {
    // Current back under limit: clear debounce
    _overStartMs = 0;
    // Do NOT auto-clear OCP when current is healthy.
    // OCP will only be cleared explicitly via clearOcpLatch() after OFF is selected.
  }

  // -------- Output Voltage Fault (dynamic): active only while under cutoff (<_outvCut) or <8V, and if >16V. --------
  if (haveOutV) {
    if (_outvBypass) {
      // Ignore all output voltage trips while bypass is active
      _outvBelowStartMs = 0;
      _outvLatched = false;
    } else {
      bool hiFault = (outV > OUTV_MAX_V);
      bool loExtreme = (outV < OUTV_MIN_V);     // <8V
      bool loSoft    = (outV < _outvCut);       // below configured cutoff (>=8V by construction)

      if (hiFault) {
        // High-side fault is immediate
        if (!_outvLatched) {
          _outvLatched = true;
          for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
        }
        _outvBelowStartMs = 0;
      } else if (loExtreme || loSoft) {
        // Low-side fault: debounce
        if (_outvBelowStartMs == 0) _outvBelowStartMs = nowMs;
        if ((nowMs - _outvBelowStartMs) >= _outvTripMs) {
          if (!_outvLatched) {
            _outvLatched = true;
            for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
          }
        }
      } else {
        // Healthy range (>= cutoff and <= 16V): clear fault immediately
        _outvBelowStartMs = 0;
        if (_outvLatched) _outvLatched = false;
      }
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
  // Now, while *any* latch is active, we force all relays OFF on every tick.
  if (_lvpLatched || _ocpLatched || _outvLatched || _relayCoilLatched) {
    for (int i = 0; i < (int)R_COUNT; ++i) relayOff(i);
    _cutsent = true;       // keep flag for backward compatibility
  } else {
    _cutsent = false;      // reset when no latches are active
  }
}

void Protector::setOcpHold(bool on) {
  _ocpHold = on;
}
