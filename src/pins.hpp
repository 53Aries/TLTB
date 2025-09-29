#pragma once

// ===== TFT on FSPI (Phase-3) =====
static constexpr int PIN_TFT_CS   = 5;
static constexpr int PIN_TFT_DC   = 14;
static constexpr int PIN_TFT_RST  = 4;
static constexpr int PIN_TFT_BL   = 15;

static constexpr int PIN_FSPI_SCK  = 36;
static constexpr int PIN_FSPI_MOSI = 35;
static constexpr int PIN_FSPI_MISO = 37;

// ===== Encoder & buttons =====
static constexpr int PIN_ENC_A    = 16;
static constexpr int PIN_ENC_B    = 18;
static constexpr int PIN_ENC_OK   = 38;
static constexpr int PIN_ENC_BACK = 39;

// ===== I2C (INA226 x2) =====
static constexpr int PIN_I2C_SDA  = 8;
static constexpr int PIN_I2C_SCL  = 9;

// ===== Relays (active-LOW) =====
static constexpr int RELAY_PIN[6] = {6, 7, 10, 21, 17, 33};

// ===== CC1101 (shares FSPI bus) =====
static constexpr int PIN_CC1101_CS   = 11;
static constexpr int PIN_CC1101_GDO0 = 12;

// Buzzer
static constexpr int PIN_BUZZER = 27;

// --- CC1101 (RF) pins ---
// Shared SCK/MISO/MOSI come from PIN_FSPI_* (same bus as TFT).
#ifndef PIN_RF_CS
  #define PIN_RF_CS   40   // CC1101 CS (chip select)
#endif
#ifndef PIN_RF_GDO0
  #define PIN_RF_GDO0 41   // CC1101 GDO0 (interrupt/status)
#endif
#ifndef PIN_RF_GDO2
  #define PIN_RF_GDO2 42   // CC1101 GDO2 (optional status)
#endif

