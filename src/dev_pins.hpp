// File Overview: Optimized ESP32-S3 WROOM-1 pin map with physically grouped connections.
// Display + encoder cluster on right side, rotary selector on left, relays on bottom.
#pragma once

// ======================= Display Board Pins =======================
// Pin order matches physical display board layout (top to bottom):
// BACK, OK, B, A, CS, DC, RES, SDA, SCL

// Rotary Encoder (top of display board)
#define PIN_ENC_BACK    1   // Physical pin 39 (top pin on display board)
#define PIN_ENC_OK      2   // Physical pin 38
#define PIN_ENC_B       43  // Physical pin 36
#define PIN_ENC_A       44  // Physical pin 37

// Encoder OK button active level (hardware drives HIGH when pressed)
#define ENC_OK_ACTIVE_LEVEL LOW

// Display (ST7735S)
#define PIN_TFT_CS      42  // Physical pin 35 (IO42 - JTAG/GPIO)
#define PIN_TFT_DC      41  // Physical pin 34 (IO41 - JTAG/GPIO)
#define PIN_TFT_RST     40  // Physical pin 33 (IO40 - JTAG/GPIO, RES)
// Backlight not used - always on or hardware controlled

// SPI (FSPI) for TFT (bottom of display board)
#define PIN_FSPI_MOSI   39  // SDA - Physical pin 32 (IO39 - JTAG/GPIO)
#define PIN_FSPI_SCK    38  // SCL - Physical pin 31 (IO38 - safe GPIO)
#define PIN_FSPI_MISO   21  // Physical pin 23 (IO21 - not used by display, bottom edge)

// ======================= Rotary 1P8T Mode Selector =======================
// Left side cluster: pins 4-11 on WROOM-1 module (safe GPIOs)
#define PIN_ROT_P1      5   // All Off       - Physical pin 5 (IO5)
#define PIN_ROT_P2      6   // RF Enable     - Physical pin 6 (IO6)
#define PIN_ROT_P3      7   // Left          - Physical pin 7 (IO7)
#define PIN_ROT_P4      15  // Right         - Physical pin 8 (IO15)
#define PIN_ROT_P5      16  // Brake         - Physical pin 9 (IO16)
#define PIN_ROT_P6      17  // Tail          - Physical pin 10 (IO17)
#define PIN_ROT_P7      18  // Marker        - Physical pin 11 (IO18)
#define PIN_ROT_P8      8   // Aux           - Physical pin 12 (IO8)

// ======================= I²C Bus (INA226 modules) =======================
// Bottom corner: pins 24-25 on WROOM-1 module (safe GPIOs with I2C support)
#define PIN_I2C_SDA     48  // Physical pin 25 (IO48 - safe GPIO)
#define PIN_I2C_SCL     47  // Physical pin 24 (IO47 - safe GPIO)
// #Source INA bridge A0

// ======================= Relays (active-low) =======================
// ALL on bottom row: pins 15-23 (physically consecutive group on bottom edge)
#define PIN_RELAY_LH       14   // Left Turn         - Physical pin 22 (IO14 - bottom edge)
#define PIN_RELAY_RH       13   // Right Turn        - Physical pin 21 (IO13 - bottom edge)
#define PIN_RELAY_BRAKE    12   // Brake Lights      - Physical pin 20 (IO12 - bottom edge)
#define PIN_RELAY_TAIL     11   // Tail Lights       - Physical pin 19 (IO11 - bottom edge)
#define PIN_RELAY_MARKER   10   // Marker Lights     - Physical pin 18 (IO10 - bottom edge)
#define PIN_RELAY_AUX      9    // Auxiliary         - Physical pin 17 (IO9 - bottom edge)
#define PIN_RELAY_SPARE    3    // Spare (unused)    - Physical pin 15 (IO3 - strapping, safe for output)

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
// Bottom corner: using GPIO45 (safe, no conflicts)
#define PIN_BUZZER      4  // Physical pin 4 (IO4)

// ======================= RF (SYN480R Receiver) =======================
// Bottom edge: using GPIO46 (strapping pin but safe for input)
// DATA pin from SYN480R — must be level-shifted to 3.3 V
// Note: Keep HIGH or floating at boot to avoid boot mode issues
#define PIN_RF_DATA     45  // Physical pin 26 (IO45 - strapping, safe for input)
