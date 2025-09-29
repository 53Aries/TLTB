#include "Rotary.hpp"

static inline uint8_t rd(int pin){ return (uint8_t)digitalRead(pin); }

// 4x4 transition table: (prev<<2|cur) -> -1,0,+1
static const int8_t kTbl[16] = {
  0, -1, +1,  0,
  +1, 0,  0, -1,
  -1, 0,  0, +1,
   0, +1, -1, 0
};

static RotaryEncoder g_inst;
RotaryEncoder& rotary(){ return g_inst; }

void RotaryEncoder::begin(int pinA, int pinB, bool usePullup, bool reversed,
                          int detentEdges, uint32_t minEdgeUs, uint32_t resetUs) {
  _pinA = pinA; _pinB = pinB;
  _reversed    = reversed;
  _detentEdges = detentEdges < 2 ? 2 : detentEdges;
  _minEdgeUs   = minEdgeUs;
  _resetUs     = resetUs;

  if (usePullup) {
    pinMode(_pinA, INPUT_PULLUP);
    pinMode(_pinB, INPUT_PULLUP);
  } else {
    pinMode(_pinA, INPUT_PULLDOWN);
    pinMode(_pinB, INPUT_PULLDOWN);
  }

  _prev       = (rd(_pinA) << 1) | rd(_pinB);
  _accum      = 0;
  _steps      = 0;
  _lastEdgeUs = micros();
}

void RotaryEncoder::poll() {
  uint32_t now = micros();
  uint8_t a = rd(_pinA);
  uint8_t b = rd(_pinB);
  uint8_t cur = (a << 1) | b;
  if (cur == _prev) return;

  uint32_t dt = now - _lastEdgeUs;
  if (dt < _minEdgeUs) { _prev = cur; return; }   // debounce
  if (dt > _resetUs)   { _accum = 0; }            // drop partial cycles on long pause

  int idx = (_prev << 2) | cur;
  int8_t d = kTbl[idx];
  _prev = cur;
  _lastEdgeUs = now;

  if (d == 0) return;
  if (_reversed) d = -d;

  _accum += d;
  if (_accum >= _detentEdges) {
    _accum = 0; _steps++;
  } else if (_accum <= -_detentEdges) {
    _accum = 0; _steps--;
  }
}

int8_t RotaryEncoder::readStep() {
  if (_steps > 0)      { _steps--; return +1; }
  else if (_steps < 0) { _steps++; return -1; }
  return 0;
}
