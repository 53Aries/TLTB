#pragma once

// ======================= SPI (FSPI) for TFT =======================
#define PIN_FSPI_SCK    37 // SCL
#define PIN_FSPI_MOSI   38 // SDA
#define PIN_FSPI_MISO   36 // not used, but must be defined

// ======================= Display (ST7735S) =======================
#define PIN_TFT_CS      41
#define PIN_TFT_DC      40
#define PIN_TFT_RST     39
#define PIN_TFT_BL      42
#define PIN_TFT_BLK     PIN_TFT_BL   // alias for code paths that use BLK

// ======================= Rotary Encoder =======================
#define PIN_ENC_A       2
#define PIN_ENC_B       1
#define PIN_ENC_OK      44
#define PIN_ENC_BACK    43

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
// Note: ESP32-S3 GPIO47/48 are input-only on many modules and are not suitable for I2C master drive.
// Provide an alternate pin option (recommended: GPIO8/9) that can actively pull low.
#ifdef I2C_ALT_PINS
#  define PIN_I2C_SDA   8
#  define PIN_I2C_SCL   9
#else
#  define PIN_I2C_SDA   47
#  define PIN_I2C_SCL   48
#endif

// ======================= Relays (active-low) =======================
#define PIN_RELAY_LH       8   // Left Turn
#define PIN_RELAY_RH       9    // Right Turn
#define PIN_RELAY_BRAKE   10    // Brake Lights
#define PIN_RELAY_TAIL    11    // Tail Lights
#define PIN_RELAY_MARKER  12    // Marker Lights
#define PIN_RELAY_AUX     13    // Auxiliary
#define PIN_RELAY_ENABLE  14    // Enable 12V buck

// Array for DisplayUI.cpp relay status logic
static const int RELAY_PIN[] = {
  PIN_RELAY_LH,
  PIN_RELAY_RH,
  PIN_RELAY_BRAKE,
  PIN_RELAY_TAIL,
  PIN_RELAY_MARKER,
  PIN_RELAY_AUX,
  PIN_RELAY_ENABLE
};

// ======================= Buzzer =======================
#define PIN_BUZZER      35

// ======================= RF (SYN480R Receiver) =======================
// DATA pin from SYN480R — must be level-shifted to 3.3 V
#define PIN_RF_DATA     21 
