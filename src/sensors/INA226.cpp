// File Overview: Configures and reads the pair of INA226 sensors used for load current
// measurement and source voltage monitoring, including calibration helpers.
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

// Calibration for SOURCE INA226 relay coil current measurement
// Default: 0.1Ω shunt, 1mA resolution (adjust via setCalibration if needed)
// 
// USAGE: Adjust calibration based on your shunt resistor:
//   Example 1: 0.1Ω shunt, 1mA resolution (default)
//     INA226_SRC::setCalibration(0.1f, 0.001f);
//   Example 2: 0.05Ω shunt, 0.5mA resolution (higher current range)
//     INA226_SRC::setCalibration(0.05f, 0.0005f);
//   Example 3: 1.0Ω shunt, 1mA resolution (very low current, better resolution)
//     INA226_SRC::setCalibration(1.0f, 0.001f);
// 
// Typical automotive relay coil: 50-150mA @ 12V
// With 7 relays max, expect up to ~700mA total coil current
//
static float   RSHUNT_SRC_OHMS   = 0.1f;          // 100 mΩ shunt
static float   CURRENT_SRC_LSB_A = 0.001f;        // 1 mA/bit
static uint16_t CALIB_SRC        = (uint16_t)((0.00512f / (CURRENT_SRC_LSB_A * RSHUNT_SRC_OHMS)) + 0.5f);
bool  INA226::PRESENT      = false;
bool  INA226_SRC::PRESENT  = false;
float INA226::OCP_LIMIT_A  = 22.0f;
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
  // AVG=4, VBUS=332µs, VSHUNT=332µs, continuous (fast OCP detection: ~2.7ms per reading)
  wr16(ADDR_LOAD, 0x00, (0b001<<9)|(0b010<<6)|(0b010<<3)|0b111);
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

// ===== SOURCE INA226 (battery voltage + relay coil current monitoring) =====
void INA226_SRC::begin(){
  ensureWire();
  PRESENT = (endTx(0x41) == 0);
  if (!PRESENT) return;

  wr16(ADDR_SRC, 0x00, 0x8000); delay(2);
  // AVG=4, VBUS=332µs, VSHUNT=332µs, continuous (fast sampling: ~2.7ms per reading)
  wr16(ADDR_SRC, 0x00, (0b001<<9)|(0b010<<6)|(0b010<<3)|0b111);
  wr16(ADDR_SRC, 0x05, CALIB_SRC);
}

void INA226_SRC::setCalibration(float rShuntOhms, float currentLsbA){
  if (!PRESENT) return;
  RSHUNT_SRC_OHMS = rShuntOhms;
  CURRENT_SRC_LSB_A = currentLsbA;
  CALIB_SRC = (uint16_t)((0.00512f / (currentLsbA * rShuntOhms)) + 0.5f);
  wr16(ADDR_SRC, 0x05, CALIB_SRC);
}

float INA226_SRC::readBusV(){
  if (!PRESENT) return 0.0f;
  uint16_t raw = rd16_or0(ADDR_SRC, 0x02);
  return raw * 1.25e-3f;
}

float INA226_SRC::readCurrentA(){
  if (!PRESENT) return 0.0f;
  int16_t raw = (int16_t)rd16_or0(ADDR_SRC, 0x04);
  return raw * CURRENT_SRC_LSB_A;
}

float INA226_SRC::getRelayCoilCurrent(){
  // Alias for readCurrentA() with clear semantic purpose
  return readCurrentA();
}

bool INA226_SRC::verifyRelayCoils(int expectedCount, float nominalCoilMa){
  if (!PRESENT) return true; // Can't verify, assume OK
  
  float measuredMa = readCurrentA() * 1000.0f;  // Convert A to mA
  float expectedMa = expectedCount * nominalCoilMa;
  
  // Allow ±40% tolerance for relay coil variation and measurement uncertainty
  float tolerance = 0.4f;
  float minExpected = expectedMa * (1.0f - tolerance);
  float maxExpected = expectedMa * (1.0f + tolerance);
  
  // Special case: if expecting zero relays, accept up to 5mA (noise floor)
  if (expectedCount == 0) {
    return (measuredMa < 5.0f);
  }
  
  return (measuredMa >= minExpected && measuredMa <= maxExpected);
}
