#include "RF.hpp"
#include <SPI.h>
#include <Preferences.h>

#ifndef RF_ENABLE
#define RF_ENABLE 1
#endif

#if RF_ENABLE
  #if __has_include(<ELECHOUSE_CC1101_SRC_DRV.h>)
    #include <ELECHOUSE_CC1101_SRC_DRV.h>
    #define HAVE_CC1101 1
  #else
    #define HAVE_CC1101 0
  #endif
#endif

namespace RF {

static bool        s_present = false;
static float       s_freqMhz = 433.92f;
static int         s_pa      = 10;
static Preferences s_nvs;

// ---- raw SPI probe of CC1101 VERSION (0x31) ----
static uint8_t spiReadReg(uint8_t addr) {
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_RF_CS, LOW);
  delayMicroseconds(1);
  (void)SPI.transfer((uint8_t)(0x80 | (addr & 0x3F))); // read
  uint8_t data = SPI.transfer((uint8_t)0x00);
  digitalWrite(PIN_RF_CS, HIGH);
  SPI.endTransaction();
  return data;
}

static bool probe_cc1101_present() {
  pinMode(PIN_RF_CS, OUTPUT);
  digitalWrite(PIN_RF_CS, HIGH);
  pinMode(PIN_RF_GDO0, INPUT_PULLUP);
  pinMode(PIN_RF_GDO2, INPUT_PULLUP);

  // quick VERSION read (0x31). 0xFF => floating bus => likely not present.
  uint8_t ver = spiReadReg(0x31);
  return (ver != 0xFF);
}

#if RF_ENABLE && HAVE_CC1101
static void applyRadioConfig() {
  ELECHOUSE_cc1101.setCCMode(1);     // library "CC mode"
  ELECHOUSE_cc1101.setModulation(2); // ASK/OOK
  ELECHOUSE_cc1101.setMHZ(s_freqMhz);
  ELECHOUSE_cc1101.setDRate(4.8);
  ELECHOUSE_cc1101.setRxBW(58);
  ELECHOUSE_cc1101.setPA(s_pa);
}
#endif

void begin() {
  // Common safe defaults
  pinMode(PIN_RF_CS, OUTPUT);
  digitalWrite(PIN_RF_CS, HIGH);
  pinMode(PIN_RF_GDO0, INPUT_PULLUP);
  pinMode(PIN_RF_GDO2, INPUT_PULLUP);

  s_nvs.begin("rf", false);

  // Compile-time disable?
#if !RF_ENABLE
  s_present = false;
  return;
#else
  // Runtime pin conflict guard (belt & suspenders)
  bool pinsConflict =
      (PIN_RF_CS == PIN_TFT_CS) || (PIN_RF_CS == PIN_ENC_A) || (PIN_RF_CS == PIN_ENC_B) ||
      (PIN_RF_GDO0 == PIN_ENC_A) || (PIN_RF_GDO0 == PIN_ENC_B) ||
      (PIN_RF_GDO2 == PIN_ENC_A) || (PIN_RF_GDO2 == PIN_ENC_B);
  if (pinsConflict) {
    ets_printf("[RF] Pin conflict detected; disabling RF init\n");
    s_present = false;
    return;
  }

  // Probe over the existing SPI (main already called SPI.begin)
  s_present = probe_cc1101_present();

  #if HAVE_CC1101
  if (s_present) {
    // hand off to driver (do not SPI.begin() again)
    ELECHOUSE_cc1101.setSpiPin(PIN_FSPI_SCK, PIN_FSPI_MISO, PIN_FSPI_MOSI, PIN_RF_CS);
    ELECHOUSE_cc1101.setGDO(PIN_RF_GDO0, PIN_RF_GDO2);

    bool ok = ELECHOUSE_cc1101.getCC1101();
    if (!ok) {
      ELECHOUSE_cc1101.Init();
      ok = ELECHOUSE_cc1101.getCC1101();
    }
    s_present = ok;
    if (s_present) applyRadioConfig();
  }
  #endif
#endif
}

void service() {
  // Reserved for future RX/TX background tasks
}

bool isPresent() { return s_present; }

bool learn(int /*buttonIdx*/) { return false; }           // implement protocol later
bool tx(int /*buttonIdx*/, bool /*onOff*/) { return false; }

void setFrequency(float mhz) {
  s_freqMhz = mhz;
#if RF_ENABLE && HAVE_CC1101
  if (s_present) ELECHOUSE_cc1101.setMHZ(mhz);
#endif
}

void setPA(int pa) {
  s_pa = pa;
#if RF_ENABLE && HAVE_CC1101
  if (s_present) ELECHOUSE_cc1101.setPA(pa);
#endif
}

} // namespace RF
