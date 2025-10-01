#pragma once

// ======================= SPI (FSPI) for TFT =======================
#define PIN_FSPI_SCK    36
#define PIN_FSPI_MOSI   35
#define PIN_FSPI_MISO   37

// ======================= Display (ST7735S) =======================
#define PIN_TFT_CS      5
#define PIN_TFT_DC      14
#define PIN_TFT_RST     4
#define PIN_TFT_BL      15
#define PIN_TFT_BLK     PIN_TFT_BL   // alias for code paths that use BLK

// ======================= Rotary Encoder =======================
#define PIN_ENC_A       16
#define PIN_ENC_B       18
#define PIN_ENC_OK      38
#define PIN_ENC_BACK    39

// ======================= Rotary 1P8T Mode Selector =======================
// All inputs use INPUT_PULLUP in setup(); LOW = active
// These are boot-safe, available on the ESP32-S3 DevKitC-1, and won’t conflict with I2C/SPI.
#define PIN_ROT_P1      11  // All Off
#define PIN_ROT_P2      12  // RF Enable
#define PIN_ROT_P3      13  // Left
#define PIN_ROT_P4      20  // Right
#define PIN_ROT_P5      40  // Brake
#define PIN_ROT_P6      42  // Tail
#define PIN_ROT_P7      1   // Marker
#define PIN_ROT_P8      2   // Aux

// ======================= I²C Bus (INA226 modules) =======================
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9

// ======================= Relays (active-low) =======================
#define PIN_RELAY_LH       6   // Left Turn
#define PIN_RELAY_RH       7   // Right Turn
#define PIN_RELAY_BRAKE   10   // Brake Lights
#define PIN_RELAY_TAIL    21   // Tail Lights
#define PIN_RELAY_MARKER  17   // Marker Lights
#define PIN_RELAY_AUX     33   // Auxiliary
// Relay 7: high-current 12V enable (active-low like others)
#define PIN_RELAY_ENABLE  34   // Enable 12V buck (choose a free GPIO)

// Array for DisplayUI.cpp relay status logic
static const int RELAY_PIN[] = {
  PIN_RELAY_LH,
  PIN_RELAY_RH,
  PIN_RELAY_BRAKE,
  PIN_RELAY_TAIL,
  PIN_RELAY_MARKER,
  PIN_RELAY_AUX
  ,PIN_RELAY_ENABLE
};

// ======================= Buzzer =======================
#define PIN_BUZZER      27

// ======================= RF (SYN480R Receiver) =======================
// DATA pin from SYN480R — must be level-shifted to 3.3 V
#define PIN_RF_DATA     41
