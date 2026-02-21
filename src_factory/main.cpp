// Factory/Recovery Firmware - Minimal OTA updater
// This runs from the factory partition and can safely update corrupted OTA partitions
#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_GFX.h>
#include <Preferences.h>
#include "esp_ota_ops.h"
#include "ota/Ota.hpp"
#include "pins.hpp"

#define NVS_NS        "tltb"
#define KEY_SSID      "wifi_ssid"
#define KEY_PASS      "wifi_pass"
#define OTA_REPO      "53Aries/TLTB"

static Adafruit_ST7735* tft = nullptr;
static Preferences prefs;

// Simple display functions
void showText(const char* line1, const char* line2 = "", const char* line3 = "") {
  tft->fillScreen(ST77XX_BLACK);
  tft->setTextColor(ST77XX_WHITE);
  tft->setTextSize(1);
  tft->setCursor(5, 20);
  tft->println(line1);
  if (line2[0]) {
    tft->setCursor(5, 35);
    tft->println(line2);
  }
  if (line3[0]) {
    tft->setCursor(5, 50);
    tft->println(line3);
  }
}

void showProgress(const char* msg, size_t written, size_t total) {
  tft->fillRect(0, 70, 160, 30, ST77XX_BLACK);
  tft->setCursor(5, 70);
  tft->println(msg);
  
  if (total > 0) {
    int pct = (written * 100) / total;
    tft->setCursor(5, 85);
    tft->printf("%d%%  %u/%u", pct, (unsigned)written, (unsigned)total);
    
    // Progress bar
    int barWidth = ((written * 140) / total);
    tft->fillRect(10, 100, barWidth, 8, ST77XX_GREEN);
    tft->drawRect(10, 100, 140, 8, ST77XX_WHITE);
  }
}

bool connectWiFi() {
  showText("RECOVERY MODE", "Connecting WiFi...", "");
  
  // Try saved credentials first
  prefs.begin(NVS_NS, true);
  String ssid = prefs.getString(KEY_SSID, "");
  String pass = prefs.getString(KEY_PASS, "");
  prefs.end();
  
  if (ssid.length() > 0) {
    showText("RECOVERY MODE", "Connecting to:", ssid.c_str());
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    
    int timeout = 15; // 15 seconds
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
      delay(1000);
      timeout--;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      showText("RECOVERY MODE", "WiFi Connected", WiFi.localIP().toString().c_str());
      delay(1000);
      return true;
    }
  }
  
  // Saved credentials failed or don't exist - scan for networks
  showText("RECOVERY MODE", "Scanning WiFi...", "Press BACK to skip");
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  WiFi.setSleep(false);
  delay(120);
  
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    showText("RECOVERY MODE", "No WiFi found", "Press BACK to skip");
    delay(2000);
    return false;
  }
  
  // Display list of networks - scroll with encoder, select with OK
  int selected = 0;
  bool choosing = true;
  
  while (choosing) {
    tft->fillScreen(ST77XX_BLACK);
    tft->setTextColor(ST77XX_WHITE);
    tft->setTextSize(1);
    tft->setCursor(5, 5);
    tft->print("Select WiFi Network:");
    
    // Show 5 networks at a time
    int startIdx = (selected / 5) * 5;
    for (int i = 0; i < 5 && (startIdx + i) < n; i++) {
      int idx = startIdx + i;
      tft->setCursor(5, 25 + i * 15);
      
      if (idx == selected) {
        tft->setTextColor(ST77XX_BLACK, ST77XX_WHITE); // Highlighted
      } else {
        tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
      }
      
      String name = WiFi.SSID(idx);
      if (name.length() > 22) name = name.substring(0, 22);
      tft->print(name);
      
      // Show signal strength
      int rssi = WiFi.RSSI(idx);
      if (rssi > -50) tft->print(" +++");
      else if (rssi > -70) tft->print(" ++");
      else tft->print(" +");
    }
    
    tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    tft->setCursor(5, 115);
    tft->print("OK=Select BACK=Skip");
    
    // Wait for button press
    delay(100);
    if (digitalRead(PIN_ENC_OK) == LOW) {
      while (digitalRead(PIN_ENC_OK) == LOW) delay(10); // Wait for release
      choosing = false;
      ssid = WiFi.SSID(selected);
    } else if (digitalRead(PIN_ENC_BACK) == LOW) {
      while (digitalRead(PIN_ENC_BACK) == LOW) delay(10);
      return false; // User cancelled
    }
    
    // Simple scroll with BACK button (double-press = move down)
    // For simplicity, auto-advance every 3 seconds
    static unsigned long lastMove = millis();
    if (millis() - lastMove > 3000) {
      selected = (selected + 1) % n;
      lastMove = millis();
    }
  }
  
  // Get password (simplified - just use a fixed test password for now)
  // TODO: Add text input keyboard
  bool isOpen = (WiFi.encryptionType(selected) == WIFI_AUTH_OPEN);
  
  if (!isOpen) {
    showText("RECOVERY MODE", "Enter password", "Using saved pass");
    // For now, keep using the old password if it exists
    // This is a limitation - proper keyboard input would be complex
    delay(2000);
  } else {
    pass = "";
  }
  
  showText("RECOVERY MODE", "Connecting to:", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  
  int timeout = 15;
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    delay(1000);
    timeout--;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    // Save credentials
    prefs.begin(NVS_NS, false);
    prefs.putString(KEY_SSID, ssid);
    prefs.putString(KEY_PASS, pass);
    prefs.end();
    
    showText("RECOVERY MODE", "WiFi Connected", WiFi.localIP().toString().c_str());
    delay(1000);
    return true;
  }
  
  showText("RECOVERY MODE", "WiFi Failed", "Press BACK to skip");
  delay(2000);
  return false;
}

bool performFactoryOTA() {
  showText("RECOVERY MODE", "Checking update...", "");
  
  // Use the OTA update function with our simple display callbacks
  Ota::Callbacks cb;
  cb.onStatus = [](const char* s) {
    showText("RECOVERY MODE", "Status:", s);
    Serial.printf("[OTA] %s\n", s);
  };
  
  cb.onProgress = [](size_t w, size_t t) {
    showProgress("Downloading...", w, t);
    Serial.printf("[OTA] %u / %u bytes\n", (unsigned)w, (unsigned)t);
  };
  
  bool success = Ota::updateFromGithubLatest(OTA_REPO, cb);
  
  if (!success) {
    showText("RECOVERY MODE", "ERROR:", "OTA failed");
    delay(3000);
  }
  
  return success;
}

void setup() {
  // Start serial FIRST before anything else
  Serial.begin(115200);
  delay(500);  // Give serial time to initialize
  
  // IMMEDIATE output to verify firmware is actually running
  Serial.println();
  Serial.println();
  Serial.println();
  for (int i = 0; i < 5; i++) {
    Serial.println("*** FACTORY FIRMWARE ALIVE ***");
    delay(100);
  }
  Serial.flush();
  
  Serial.println("\n\n\n========================================");
  Serial.println("FACTORY RECOVERY FIRMWARE STARTING");
  Serial.println("========================================");
  Serial.flush();
  
  // CRITICAL: Disable all relays FIRST to prevent accidental activation
  // Relays are active-low, so HIGH = OFF
  Serial.println("[Factory] Disabling all relays...");
  Serial.flush();
  pinMode(PIN_RELAY_LH, OUTPUT);     digitalWrite(PIN_RELAY_LH, HIGH);
  pinMode(PIN_RELAY_RH, OUTPUT);     digitalWrite(PIN_RELAY_RH, HIGH);
  pinMode(PIN_RELAY_BRAKE, OUTPUT);  digitalWrite(PIN_RELAY_BRAKE, HIGH);
  pinMode(PIN_RELAY_TAIL, OUTPUT);   digitalWrite(PIN_RELAY_TAIL, HIGH);
  pinMode(PIN_RELAY_MARKER, OUTPUT); digitalWrite(PIN_RELAY_MARKER, HIGH);
  pinMode(PIN_RELAY_AUX, OUTPUT);    digitalWrite(PIN_RELAY_AUX, HIGH);
  Serial.println("[Factory] Relays disabled");
  Serial.flush();
  
  // CRITICAL: Reset boot partition to OTA_0 immediately on factory entry
  // This ensures we never get stuck in factory mode after power cycle
  // If OTA succeeds, Update.end() will set the new partition as boot
  Serial.println("[Factory] Resetting boot partition to OTA_0...");
  Serial.flush();
  const esp_partition_t* ota0 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (ota0) {
    esp_ota_set_boot_partition(ota0);
    Serial.println("[Factory] Boot partition reset to OTA_0 for safety");
  }
  
  Serial.println("[Factory] Starting display initialization...");
  Serial.flush();
  
  Serial.println("\n\n========================================");
  Serial.println("FACTORY RECOVERY MODE");
  Serial.println("========================================");
  Serial.println("All relays disabled for safety");
  
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    Serial.printf("Running from: %s (0x%x)\n", running->label, running->address);
  }
  
  // Initialize display hardware - EXACT sequence from main.cpp
  Serial.println("[Factory] Configuring display pins...");
  Serial.flush();
  // TFT backlight not used (GPIO 42 repurposed for INA ALERT in main firmware)
  // Display runs without backlight control
  
  // Configure all TFT pins as outputs before init
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_TFT_CS, HIGH);  // CS idle high (deselected)
  pinMode(PIN_TFT_DC, OUTPUT);
  pinMode(PIN_TFT_RST, OUTPUT);
  
  // Configure SPI pins explicitly
  pinMode(PIN_FSPI_SCK, OUTPUT);
  pinMode(PIN_FSPI_MOSI, OUTPUT);
  pinMode(PIN_FSPI_MISO, INPUT);
  
  // Start SPI bus
  Serial.println("[Factory] Starting SPI bus...");
  Serial.flush();
  SPI.begin(PIN_FSPI_SCK, PIN_FSPI_MISO, PIN_FSPI_MOSI, PIN_TFT_CS);
  delay(30);  // Allow peripheral settle (matches main.cpp)
  Serial.println("[Factory] SPI bus ready");
  Serial.flush();
  
  // TFT reset + init - strong hardware reset timing
  digitalWrite(PIN_TFT_RST, HIGH); delay(50);
  digitalWrite(PIN_TFT_RST, LOW);  delay(120);
  digitalWrite(PIN_TFT_RST, HIGH); delay(150);
  
  // Create TFT object and initialize - EXACTLY as main.cpp
  Serial.println("[Factory] Creating TFT object...");
  Serial.flush();
  tft = new Adafruit_ST7735(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
  Serial.println("[Factory] Setting SPI speed...");
  Serial.flush();
  tft->setSPISpeed(8000000UL);
  Serial.println("[Factory] Initializing display controller...");
  Serial.flush();
  tft->initR(INITR_BLACKTAB);
  Serial.println("[Factory] Setting rotation...");
  Serial.flush();
  tft->setRotation(1);  // MUST match main app rotation 1
  Serial.println("[Factory] Clearing screen...");
  Serial.flush();
  tft->fillScreen(ST77XX_BLACK);
  delay(100);  // CRITICAL: Ensure fillScreen SPI operations complete
  Serial.println("[Factory] Display initialized");
  Serial.flush();
  
  // Button setup
  pinMode(PIN_ENC_OK, INPUT_PULLUP);
  pinMode(PIN_ENC_BACK, INPUT_PULLUP);
  
  // Draw recovery UI
  Serial.println("[Factory] Drawing recovery UI...");
  Serial.flush();
  showText("RECOVERY MODE", "Factory Partition", "Press OK to update");
  Serial.println("[Factory] UI drawn, display ready");
  Serial.flush();
  
  // TFT backlight not used - display runs without it
  delay(100);
  
  delay(2000);
  
  // Wait for OK button press
  bool waitingForUser = true;
  unsigned long startWait = millis();
  const unsigned long AUTO_TIMEOUT = 10000; // 10 seconds
  
  while (waitingForUser) {
    if (digitalRead(PIN_ENC_OK) == LOW) {
      // Wait for release
      while (digitalRead(PIN_ENC_OK) == LOW) delay(10);
      waitingForUser = false;
      break;
    }
    
    if (digitalRead(PIN_ENC_BACK) == LOW) {
      // Cancel - reboot to OTA partition
      showText("RECOVERY MODE", "Cancelled", "Rebooting...");
      delay(1000);
      
      // Try to boot to OTA_0
      const esp_partition_t* ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
      if (ota0) {
        esp_ota_set_boot_partition(ota0);
      }
      ESP.restart();
    }
    
    // Auto-start after timeout
    if (millis() - startWait > AUTO_TIMEOUT) {
      showText("RECOVERY MODE", "Auto-starting...", "");
      delay(500);
      waitingForUser = false;
    }
    
    delay(100);
  }
  
  // Perform the update
  if (connectWiFi()) {
    bool success = performFactoryOTA();
    
    if (success) {
      // OTA code will restart automatically
      while(1) delay(1000);
    } else {
      showText("RECOVERY MODE", "FAILED", "Press BACK to retry");
      
      // Wait for button press
      while (true) {
        if (digitalRead(PIN_ENC_BACK) == LOW) {
          // Reset boot partition before restarting
          const esp_partition_t* ota0 = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
          if (ota0) {
            esp_ota_set_boot_partition(ota0);
          }
          ESP.restart();
        }
        delay(100);
      }
    }
  } else {
    showText("RECOVERY MODE", "WiFi FAILED", "Press BACK to retry");
    
    // Wait for button press
    while (true) {
      if (digitalRead(PIN_ENC_BACK) == LOW) {
        // Reset boot partition before restarting
        const esp_partition_t* ota0 = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (ota0) {
          esp_ota_set_boot_partition(ota0);
        }
        ESP.restart();
      }
      delay(100);
    }
  }
}

void loop() {
  // Nothing - we either update and restart, or wait for user input
  delay(1000);
}
