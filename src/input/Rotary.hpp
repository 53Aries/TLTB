// File Overview: Declares the lightweight RotaryEncoder helper that the UI polls for
// high-resolution knob input without relying on interrupts.
#pragma once
#include <Arduino.h>

// Non-ISR rotary decoder (quadrature). Call poll() frequently (every 0.5–2 ms).
class RotaryEncoder {
public:
  // detentEdges: 4 = one step per full cycle (most stable), 2 = half-cycle detents
  void begin(int pinA, int pinB,
             bool usePullup   = true,
             bool reversed    = false,
             int  detentEdges = 2,
             uint32_t minEdgeUs = 700,   // ignore edges closer than this (debounce)
             uint32_t resetUs   = 8000); // drop partial cycle after idle gap

  // Call in loop (>=500 Hz ideal)
  void poll();

  // Returns -1, 0, or +1 (consumes one queued step)
  int8_t readStep();

private:
  int _pinA = -1, _pinB = -1;
  bool _reversed = false;
  int  _detentEdges = 2;
  uint32_t _minEdgeUs = 700;
  uint32_t _resetUs   = 8000;

  uint8_t  _prev = 0xFF;   // last A/B state
  int8_t   _accum = 0;     // accumulates table deltas
  int32_t  _steps = 0;     // queued steps
  uint32_t _lastEdgeUs = 0;
};

// Singleton accessor (keeps usage simple)
RotaryEncoder& rotary();
