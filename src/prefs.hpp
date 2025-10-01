#pragma once
#include <Preferences.h>

extern Preferences prefs;

static constexpr const char* NVS_NS          = "tltb";
static constexpr const char* KEY_WIFI_SSID   = "wifi_ssid";
static constexpr const char* KEY_WIFI_PASS   = "wifi_pass";
static constexpr const char* KEY_BRIGHT      = "bright";
static constexpr const char* KEY_LV_CUTOFF   = "lv_cut";
static constexpr const char* KEY_OCP         = "ocp_a";
static constexpr const char* RF_PREF_KEYS[6] = {"rf_left","rf_right","rf_brake","rf_tail","rf_marker","rf_aux"};

static constexpr const char* KEY_OTA_URL     = "ota_url";
// Persisted RF bit-bang orientation key (0=none,1=normal,2=swapped)
static constexpr const char* KEY_RF_BB_ORIENT = "rf_bb_or";
#ifndef OTA_LATEST_ASSET_URL
#define OTA_LATEST_ASSET_URL ""
#endif
