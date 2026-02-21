// File Overview: Centralized ESP32-S3 pin map covering the TFT, encoder, rotary switch,
// INA226 sensors, relays, buzzer, and RF receiver connections.
// 2/20/2026 update pins for ESP board v1.5.2
#pragma once

// ======================= SPI (FSPI) for TFT =======================
#define PIN_FSPI_SCK    36 // SCL
#define PIN_FSPI_MOSI   37 // SDA
#define PIN_FSPI_MISO   36 // not used, but must be defined

// ======================= Display (ST7735S) =======================
#define PIN_TFT_CS      40
#define PIN_TFT_DC      39
#define PIN_TFT_RST     38
// TFT backlight not used - display runs without it
// #define PIN_TFT_BL      42  // repurposed for INA226 ALERT

// ======================= Rotary Encoder =======================
#define PIN_ENC_A       42
#define PIN_ENC_B       2
#define PIN_ENC_OK      1
#define PIN_ENC_BACK    43

// Encoder OK button active level (hardware drives HIGH when pressed)
#define ENC_OK_ACTIVE_LEVEL LOW

// ======================= Rotary 1P8T Mode Selector =======================
#define PIN_ROT_P1      4  // All Off
#define PIN_ROT_P2      5  // RF Enable
#define PIN_ROT_P3      6  // Left
#define PIN_ROT_P4      7  // Right
#define PIN_ROT_P5      15  // Brake
#define PIN_ROT_P6      16  // Tail
#define PIN_ROT_P7      17  // Marker
#define PIN_ROT_P8      18  // Aux

// ======================= I²C Bus (INA226 modules) =======================
#define PIN_I2C_SDA     47
#define PIN_I2C_SCL     48
// #Source INA bridge A0

// ======================= INA226 ALERT Pin =======================
// LOAD INA226 ALERT output for extreme overcurrent detection (>30A)
// Triggers before buck converter shutdown to log short circuit event
#define PIN_INA_LOAD_ALERT  41

// ======================= Relays (ULN2803 active-high) =======================
#define PIN_RELAY_LH       9   // Left Turn
#define PIN_RELAY_RH       10   // Right Turn
#define PIN_RELAY_BRAKE   11    // Brake Lights
#define PIN_RELAY_TAIL    12    // Tail Lights
#define PIN_RELAY_MARKER  13    // Marker Lights
#define PIN_RELAY_AUX     14    // Auxiliary

// Array for DisplayUI.cpp relay status logic
static const int RELAY_PIN[] = {
  PIN_RELAY_LH,
  PIN_RELAY_RH,
  PIN_RELAY_BRAKE,
  PIN_RELAY_TAIL,
  PIN_RELAY_MARKER,
  PIN_RELAY_AUX
};

// ======================= Buzzer =======================
#define PIN_BUZZER      35

// ======================= RF (SYN480R Receiver) =======================
// DATA pin from SYN480R — must be level-shifted to 3.3 V
#define PIN_RF_DATA     21 
