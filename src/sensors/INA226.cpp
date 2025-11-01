#include "INA226.hpp"
#include "pins.hpp"
#include "../prefs.hpp"
#include <math.h>
#include <Arduino.h>
#include <Wire.h>

// ===== Config =====
static constexpr uint8_t ADDR_LOAD = 0x40;
static constexpr uint8_t ADDR_SRC  = 0x41;

// Calibration for LOAD INA226 current measurement
// Shunt: 40A / 75mV -> R_shunt = 0.075 / 40 = 1.875 mΩ
static constexpr float   RSHUNT_OHMS    = 0.001875f;
static constexpr float   CURRENT_LSB_A  = 0.001f;   // 1 mA/bit
// Calibration register formula (per datasheet): CAL = 0.00512 / (Current_LSB * R_shunt)
static constexpr uint16_t CALIB         = (uint16_t)((0.00512f / (CURRENT_LSB_A * RSHUNT_OHMS)) + 0.5f); // ≈ 2731 (0x0AAB)
bool  INA226::PRESENT      = false;
bool  INA226_SRC::PRESENT  = false;
float INA226::OCP_LIMIT_A  = 20.0f;
static bool s_invertLoad = false;   // persisted via NVS

// --- I2C bring-up (once) ---
static bool s_wireInited = false;
static void ensureWire() {
  if (s_wireInited) return;
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000); // 400 kHz
  Wire.setTimeOut(50);                           // 50 ms ceiling
  s_wireInited = true;
}

// --- helpers ---
static uint8_t endTx(uint8_t addr){
  Wire.beginTransmission(addr);
  return Wire.endTransmission(true);
}

static void wr16(uint8_t addr, uint8_t reg, uint16_t val){
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write((uint8_t)(val >> 8));
  Wire.write((uint8_t)(val & 0xFF));
  Wire.endTransmission(true);
}

static uint16_t rd16_or0(uint8_t addr, uint8_t reg){
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  // Explicit overload to avoid ambiguity on ESP32 (core 2.0.14)
  size_t got = Wire.requestFrom((uint16_t)addr, (uint8_t)2, (bool)true);
  if (got != 2) return 0;
  uint16_t v = (Wire.read() << 8) | Wire.read();
  return v;
}

// ===== LOAD INA226 (current) =====
void INA226::begin(){
  ensureWire();
  PRESENT = (endTx(0x40) == 0);
  if (!PRESENT) return;

  wr16(ADDR_LOAD, 0x00, 0x8000); delay(2);
  // AVG=16, VBUS=1.1ms, VSHUNT=1.1ms, continuous
  wr16(ADDR_LOAD, 0x00, (0b010<<9)|(0b100<<6)|(0b100<<3)|0b111);
  wr16(ADDR_LOAD, 0x05, CALIB);
  // Load invert preference
  s_invertLoad = prefs.getBool(KEY_CURR_INV, false);
}

void INA226::setOcpLimit(float amps){ OCP_LIMIT_A = amps; }

float INA226::readBusV(){
  if (!PRESENT) return 0.0f;
  uint16_t raw = rd16_or0(ADDR_LOAD, 0x02);
  return raw * 1.25e-3f;
}

float INA226::readCurrentA(){
  if (!PRESENT) return 0.0f;
  int16_t raw = (int16_t)rd16_or0(ADDR_LOAD, 0x04);
  float a = raw * CURRENT_LSB_A;
  return s_invertLoad ? -a : a;
}

bool INA226::ocpActive(){
  if (!PRESENT) return false;
  float a = fabsf(readCurrentA());
  return (a >= OCP_LIMIT_A);
}

void INA226::setInvert(bool on){
  s_invertLoad = on;
  prefs.putBool(KEY_CURR_INV, on);
}

bool INA226::getInvert(){ return s_invertLoad; }

// ===== SOURCE INA226 (battery voltage for LVP) =====
void INA226_SRC::begin(){
  ensureWire();
  PRESENT = (endTx(0x41) == 0);
  if (!PRESENT) return;

  wr16(ADDR_SRC, 0x00, 0x8000); delay(2);
  wr16(ADDR_SRC, 0x00, (0b010<<9)|(0b100<<6)|(0b100<<3)|0b111);
  wr16(ADDR_SRC, 0x05, CALIB);
}

float INA226_SRC::readBusV(){
  if (!PRESENT) return 0.0f;
  uint16_t raw = rd16_or0(ADDR_SRC, 0x02);
  return raw * 1.25e-3f;
}
