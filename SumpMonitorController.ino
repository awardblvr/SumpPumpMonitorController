/*
 * Sump Monitor Controller - ESP32-S2 Version
 *
 * Adafruit Feather ESP32-S2 TFT based sump pump controller
 * with water level monitoring, automatic pump control, and alarm system
 *
 * Features:
 * - ADS1115 ADC for water level sensing with gain configuration
 * - Automatic pump control with timeout alarm
 * - WiFi connection with static IP and OTA update support
 * - Improved error handling and startup display
 * - Initial sensor reading before pump control starts
 * - Built-in 240x135 ST7789 TFT display
 * - Built-in NeoPixel status indicator
 *
 * Hardware:
 * - Adafruit Feather ESP32-S2 TFT board
 * - ADS1115 ADC (I2C address 0x48) via STEMMA QT
 * - 0-10V water level sensor (with voltage divider)
 * - 5V relay module (active-low) on GPIO 5
 * - Piezo beeper on GPIO 6
 * - 2 push buttons on GPIO 9 and GPIO 10
 * - Built-in NeoPixel on GPIO 33
 */

#include <WiFi.h>
#include <time.h>
#include <ArduinoOTA.h>         // Over-The-Air updates
#include <Adafruit_GFX.h>       // Core graphics library
#include <Adafruit_ST7789.h>    // Hardware-specific library for ST7789
#include <SPI.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <Wire.h>
#include "secrets.h"            // WiFi/OTA credentials (git-ignored; copy secrets.h.example)

// Firmware version - shown on the idle display (under the clock) and on the boot screen.
// Bump this every time you upload a new build so you can tell at a glance what is running.
#define FW_VERSION "1.0"

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);  // Display
Adafruit_ADS1115 ads;       // ADC
Adafruit_NeoPixel pixels(1, 33, NEO_GRB + NEO_KHZ800);  // Built-in NeoPixel on GPIO33 (1 pixel)
Adafruit_NeoPixel externalPixel(2, 11, NEO_GRB + NEO_KHZ800);  // External NeoPixels on GPIO11 (2 pixels)

Preferences prefs;          // Non-volatile storage

// Hardware status flags
bool ads_present = false;   // Track if ADS1115 is connected
bool builtin_neo_present = true;  // Set to false to disable built-in NeoPixel
bool external_neo_present = true;  // Set to false if external NeoPixel not installed
bool ota_in_progress = false;  // Track if OTA update is in progress

// WiFi and Time (PST: UTC-8)
// Credentials live in secrets.h (git-ignored). Copy secrets.h.example to secrets.h and edit.
const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASSWORD;
const long gmtOffset_sec = -8 * 3600;  // PST
const int daylightOffset_sec = 0;      // No DST adjustment
const char* ntpServer = "pool.ntp.org";
bool time_synced = false;  // Track if NTP sync succeeded

// Static IP Configuration (DISABLED - using DHCP)
// Uncomment these lines and the WiFi.config() call below to use static IP
// IPAddress local_IP(192, 168, 1, 100);     // Change to match your network
// IPAddress gateway(192, 168, 1, 1);        // Your router IP
// IPAddress subnet(255, 255, 255, 0);       // Subnet mask
// IPAddress primaryDNS(8, 8, 8, 8);         // Google DNS (optional)
// IPAddress secondaryDNS(8, 8, 4, 4);       // Google DNS (optional)

// OTA Configuration
const char* ota_hostname = SECRET_OTA_HOSTNAME;  // Name shown in Arduino IDE (from secrets.h)
const char* ota_password = SECRET_OTA_PASSWORD;  // Password for OTA updates (from secrets.h)

// Pin Definitions for Adafruit Feather ESP32-S2 TFT
#define RELAY_PIN 5      // Pump relay (active high) - Connect to external GPIO5
#define RELAY1_PIN 12    // N.O. Relay 1 (active high) - Connect to external GPIO12
#define RELAY2_PIN 13    // N.O. Relay 2 (active high) - Connect to external GPIO13
#define BEEPER_PIN 6     // Beeper (active high) - Connect to external GPIO6
#define BTN1_PIN 9       // Manual toggle button - Connect to GPIO9 (pullup enabled)
#define BTN2_PIN 10      // Alarm reset button - Connect to GPIO10 (pullup enabled)
#define MISC_INPUT1_PIN 14  // Misc Input 1 (A4) - Connect to GPIO14 (pullup enabled)
#define MISC_INPUT2_PIN 8   // Misc Input 2 (A5) - Connect to GPIO8 (pullup enabled)
#define NEO_PIN 33       // Built-in NeoPixel (onboard, no external connection needed)
#define I2C_SDA 42       // I2C SDA - Connect ADS1115 SDA to GPIO42 header pin
#define I2C_SCL 41       // I2C SCL - Connect ADS1115 SCL to GPIO41 header pin
// Note: TFT_I2C_POWER (GPIO 21), TFT_BACKLITE (GPIO 45), and NEOPIXEL_POWER (GPIO 34) are defined in board files

// Pump Logic
#define TRIGGER_INCHES 5.0
#define OFF_INCHES 1.0
#define SENSOR_OFFSET_INCHES 0.0  // Sensor offset (was 1.5")
#define TIMEOUT_SEC 240  // 4 minutes max runtime
#define PAUSE_SEC 60     // 1 minute pause after timeout
#define EXTENSION_SEC 5 // 5 second extension after reaching OFF_INCHES

// Debug Configuration
#define enaDbgSer 0  // 0 = no serial output, 1 = enable serial debug output

// Water Level Indicator Configuration for External NeoPixel 0
// This table defines flash patterns based on water level ranges
struct WaterLevelIndicator {
  float min_inches;      // Minimum water level (inclusive)
  float max_inches;      // Maximum water level (exclusive, use 999 for unlimited)
  uint8_t red;           // Red component (0-255)
  uint8_t green;         // Green component (0-255)
  uint8_t blue;          // Blue component (0-255)
  uint8_t brightness;    // Brightness percentage (0-100)
  uint8_t flash_count;   // Number of blinks in sequence
  uint8_t flash_hz;      // Blink frequency in Hz (2Hz or 4Hz)
};

// Water level flash table for external pixel 0
// Pattern: flash_count blinks at flash_hz, then 2 seconds OFF, repeat
const WaterLevelIndicator WATER_LEVEL_TABLE[] = {
  // min,  max,   R,   G,   B,  bright%, count, Hz
  {  0.0,  1.0,   0,   0,   0,     0,      0,    0  },  // Below 1": Use normal status colors
  {  1.0,  2.0, 128,   0, 128,    60,      1,    2  },  // 1-2": 1 blink Purple 60% @ 2Hz
  {  2.0,  3.0, 128,   0, 128,    80,      2,    2  },  // 2-3": 2 blinks Purple 80% @ 2Hz
  {  3.0,  4.0, 128,   0, 128,    80,      3,    2  },  // 3-4": 3 blinks Purple 80% @ 2Hz
  {  4.0,  5.0, 255, 255,   0,    80,      4,    2  },  // 4-5": 4 blinks Yellow 80% @ 2Hz
  {  5.0,  6.0, 255, 255,   0,   100,      5,    2  },  // 5-6": 5 blinks Yellow 100% @ 2Hz
  {  6.0,  7.0, 255, 165,   0,    70,      6,    2  },  // 6-7": 6 blinks Orange 70% @ 2Hz
  {  7.0, 999.0, 255, 165,   0,   100,      7,    4  }   // >7":  7 blinks Orange 100% @ 4Hz
};
const uint8_t WATER_LEVEL_TABLE_SIZE = sizeof(WATER_LEVEL_TABLE) / sizeof(WATER_LEVEL_TABLE[0]);

bool pump_on = false;
unsigned long pump_start_ms = 0;
unsigned long pump_pause_start_ms = 0;
bool pump_in_pause = false;
bool pump_extension_active = false;
unsigned long pump_extension_start_ms = 0;
bool alarm_active = false;

// Daily maintenance cycle (7:00 AM) - not logged in statistics
bool maintenance_mode = false;  // True when in 7AM maintenance cycle
unsigned long maintenance_start_ms = 0;
uint32_t last_maintenance_day = 0;  // Track which day we ran maintenance
bool maintenance_extension_active = false;  // True when in 3-second extension
unsigned long maintenance_extension_start_ms = 0;
const uint8_t MAINTENANCE_HOUR = 7;  // 7:00 AM local time
const float MAINTENANCE_TRIGGER_INCHES = 0.5;  // Run if >= 0.5 inches
const float MAINTENANCE_TARGET_INCHES = 0.25;  // Stop at <= 0.25 inches
const unsigned long MAINTENANCE_MAX_SEC = 15;  // Max 15 seconds
const unsigned long MAINTENANCE_EXTENSION_SEC = 3;  // 3 second extension after reaching target

// Daily clock resync (6:45 AM)
uint32_t last_clock_sync_day = 0;  // Track which day we resynced clock
const uint8_t CLOCK_SYNC_HOUR = 6;   // 6:00 AM hour
const uint8_t CLOCK_SYNC_MINUTE = 45; // 45 minutes (6:45 AM)

// NTP retry configuration
unsigned long boot_time_ms = 0;  // Track when device booted
unsigned long last_ntp_retry_ms = 0;  // Last NTP retry attempt
const unsigned long NTP_RETRY_INTERVAL_EARLY_MS = 120000;  // 2 minutes for first 10 minutes
const unsigned long NTP_RETRY_INTERVAL_LATE_MS = 900000;   // 15 minutes after first 10 minutes
const unsigned long NTP_EARLY_PERIOD_MS = 600000;  // 10 minutes after boot

// Manual pump control with timed mode
bool manual_pump_active = false;
unsigned long manual_pump_end_ms = 0;  // When manual timer expires
unsigned long manual_press_window_start_ms = 0;  // 10-second window start
uint8_t manual_press_count = 0;  // Count presses (max 8)
unsigned long btn1_press_start_ms = 0;  // For long press detection
const uint8_t MANUAL_MAX_PRESSES = 8;  // Max 8 presses
const unsigned long MANUAL_PRESS_WINDOW_MS = 10000;  // 10 seconds
const unsigned long MANUAL_TIME_PER_PRESS_MS = 20000;  // 20 seconds per press
const unsigned long MANUAL_LONG_PRESS_MS = 4000;  // 4 second long press

// Relay states
bool relay1_on = false;
bool relay2_on = false;

// Averaging (no averaging - single reading)
float readings[1];
int read_idx = 0;
bool buffer_full = false;
float current_avg_inches = 0.0;

// Storage
uint32_t last_start = 0;
uint32_t last_dur_sec = 0;
uint32_t daily_secs[5] = {0};  // Index 0: today, 1: yesterday, ..., 4: 5 days ago (in seconds)
uint32_t last_day_num = 0;
unsigned long last_hourly_save = 0;  // Track last hourly save time

// Display
unsigned long last_display_update = 0;
const unsigned long DISPLAY_INTERVAL = 1000;  // 1s

// Button 2 reset mechanism (10 presses within 30 seconds to reset stats)
const uint8_t RESET_PRESS_COUNT = 10;
const unsigned long RESET_TIMEOUT_MS = 30000;  // 30 seconds
uint8_t btn2_reset_count = 0;
unsigned long btn2_reset_start = 0;

// Reboot mechanism (hold both buttons for 5 seconds)
const unsigned long REBOOT_HOLD_TIME_MS = 5000;  // 5 seconds
unsigned long both_buttons_hold_start = 0;
bool both_buttons_held = false;
bool reboot_triggered = false;

/**
 * Debug helper: Print message to both Serial and TFT display
 */
void debugPrint(const char* msg, uint16_t color = ST77XX_WHITE) {
  if (enaDbgSer) Serial.println(msg);
  tft.setTextColor(color);
  tft.println(msg);
}

/**
 * Attempt to sync time with NTP server
 * Returns true if successful, false if failed
 */
bool attemptNTPSync() {
  // First check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    if (enaDbgSer) Serial.println("NTP retry: WiFi not connected, attempting reconnect...");
    WiFi.begin(ssid, password);

    // Wait up to 15 seconds for WiFi
    int wifi_attempts = 0;
    const int MAX_WIFI_ATTEMPTS = 30;  // 15 seconds max

    while (WiFi.status() != WL_CONNECTED && wifi_attempts < MAX_WIFI_ATTEMPTS) {
      delay(500);
      wifi_attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (enaDbgSer) Serial.println("NTP retry: WiFi reconnection failed");
      return false;
    }
    if (enaDbgSer) Serial.println("NTP retry: WiFi reconnected");
  }

  // WiFi connected, now try NTP sync
  if (enaDbgSer) Serial.println("NTP retry: Attempting time sync...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  delay(2000);  // Allow time for NTP sync

  time_t now;
  time(&now);
  if (now > 1000000000) {  // Valid timestamp check
    time_synced = true;
    if (enaDbgSer) Serial.println("NTP retry: Time sync SUCCESS");
    return true;
  } else {
    if (enaDbgSer) Serial.println("NTP retry: Time sync failed");
    return false;
  }
}


void setup() {
  // Record boot time for NTP retry logic
  boot_time_ms = millis();

  Serial.begin(115200);
  delay(500);
  if (enaDbgSer) {
    Serial.println("\n\nSump Monitor Controller Starting...");
    Serial.println("ESP32-S2 TFT Version");
  }

  // CRITICAL: Turn on TFT/I2C power supply FIRST!
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  if (enaDbgSer) Serial.println("TFT/I2C power enabled (GPIO 21)");
  delay(10);

  // CRITICAL: Turn on NeoPixel power (only if enabled)
  pinMode(NEOPIXEL_POWER, OUTPUT);
  if (builtin_neo_present) {
    digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
    if (enaDbgSer) Serial.println("NeoPixel power enabled (GPIO 34)");
  } else {
    digitalWrite(NEOPIXEL_POWER, !NEOPIXEL_POWER_ON);  // Turn OFF
    if (enaDbgSer) Serial.println("NeoPixel power disabled (GPIO 34)");
  }
  delay(10);

  // CRITICAL: Enable TFT backlight
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  if (enaDbgSer) Serial.println("TFT backlight enabled (GPIO 45)");
  delay(10);

  // Initialize I2C (must be after TFT_I2C_POWER is enabled)
  Wire.begin(I2C_SDA, I2C_SCL);
  if (enaDbgSer) Serial.printf("I2C initialized on SDA=%d, SCL=%d\n", I2C_SDA, I2C_SCL);

  // Configure GPIO pins
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);  // Pump relay off (active high)
  pinMode(RELAY1_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, LOW);  // Relay 1 off (active high)
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY2_PIN, LOW);  // Relay 2 off (active high)
  pinMode(BEEPER_PIN, OUTPUT);
  digitalWrite(BEEPER_PIN, LOW);
  pinMode(BTN1_PIN, INPUT);  // External 10k pull-up resistor
  pinMode(BTN2_PIN, INPUT);  // External 10k pull-up resistor
  pinMode(MISC_INPUT1_PIN, INPUT_PULLUP);
  pinMode(MISC_INPUT2_PIN, INPUT_PULLUP);
  if (enaDbgSer) Serial.println("GPIO pins configured");

  // Initialize display
  if (enaDbgSer) Serial.println("Initializing TFT display...");
  tft.init(135, 240);  // ST7789 135x240
  if (enaDbgSer) Serial.println("TFT init() completed");

  tft.setRotation(1);  // Correct orientation (landscape)
  if (enaDbgSer) Serial.println("TFT rotation set to 1 (landscape)");

  tft.fillScreen(ST77XX_BLACK);
  if (enaDbgSer) Serial.println("TFT screen cleared to black");

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);  // Smaller text for debug messages
  tft.setCursor(0, 0);
  if (enaDbgSer) Serial.println("TFT text parameters set");

  debugPrint("Sump Monitor v" FW_VERSION, ST77XX_CYAN);
  debugPrint("ESP32-S2 TFT");
  debugPrint("");
  debugPrint("Initializing...");
  if (enaDbgSer) Serial.println("Display initialized successfully");

  // Initialize ADS1115 (optional - continue if missing)
  debugPrint("Checking ADS1115...");
  if (ads.begin()) {
    ads_present = true;
    ads.setGain(GAIN_ONE);  // ±4.096V range (1 bit = 0.125mV)
    debugPrint("ADC: OK (GAIN_ONE)", ST77XX_GREEN);
  } else {
    ads_present = false;
    debugPrint("ADC: NOT FOUND!", ST77XX_YELLOW);
    debugPrint("Using dummy values");
    delay(1000);
  }

  // Initialize built-in NeoPixel (1 pixel on GPIO 33)
  if (builtin_neo_present) {
    pixels.begin();
    pixels.setBrightness(40);  // 20% dimmer than external (50 -> 40)
    pixels.setPixelColor(0, pixels.Color(255, 255, 0));  // Yellow during init
    pixels.show();
    debugPrint("NeoPixel: ON", ST77XX_GREEN);
  }

  // Initialize external NeoPixels (optional)
  if (external_neo_present) {
    externalPixel.begin();
    externalPixel.setBrightness(50);  // Match built-in brightness
    externalPixel.setPixelColor(0, externalPixel.Color(76, 76, 0));  // Yellow during init (30% brightness)
    externalPixel.setPixelColor(1, externalPixel.Color(255, 255, 0));  // Yellow during init (full brightness)
    externalPixel.show();
    debugPrint("Ext NeoPixels: OK (2x)", ST77XX_GREEN);
  }

  // WiFi Connection with timeout
  debugPrint("");
  debugPrint("Connecting WiFi...");

  // Using DHCP (router assigns IP automatically)
  // For static IP, uncomment the variables above and this block:
  // if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
  //   Serial.println("Static IP configuration failed");
  // }

  WiFi.begin(ssid, password);

  // CORRECTION: Add timeout to prevent indefinite blocking
  int wifi_attempts = 0;
  const int MAX_WIFI_ATTEMPTS = 30;  // 15 seconds max

  while (WiFi.status() != WL_CONNECTED && wifi_attempts < MAX_WIFI_ATTEMPTS) {
    delay(500);
    if (enaDbgSer) Serial.print(".");
    wifi_attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (enaDbgSer) {
      Serial.println();
      Serial.print("WiFi connected! IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("MAC Address: ");
      Serial.println(WiFi.macAddress());
    }
    debugPrint("WiFi: CONNECTED", ST77XX_GREEN);

    char ip_buf[32];
    snprintf(ip_buf, sizeof(ip_buf), "IP: %s", WiFi.localIP().toString().c_str());
    debugPrint(ip_buf);

    char mac_buf[32];
    snprintf(mac_buf, sizeof(mac_buf), "MAC: %s", WiFi.macAddress().c_str());
    debugPrint(mac_buf);

    // Sync time with NTP
    debugPrint("Syncing time...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    delay(2000);  // Allow time for NTP sync

    time_t now;
    time(&now);
    if (now > 1000000000) {  // Valid timestamp check
      time_synced = true;
      debugPrint("Time: SYNCED", ST77XX_GREEN);
    } else {
      debugPrint("Time: Failed", ST77XX_YELLOW);
    }

    // Configure OTA (Over-The-Air updates)
    ArduinoOTA.setHostname(ota_hostname);
    ArduinoOTA.setPassword(ota_password);

    ArduinoOTA.onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else {  // U_SPIFFS
        type = "filesystem";
      }
      if (enaDbgSer) Serial.println("Start updating " + type);

      // CRITICAL: Set OTA flag and turn off pump before firmware update
      ota_in_progress = true;
      if (pump_on) {
        digitalWrite(RELAY_PIN, LOW);  // Turn pump OFF immediately
        pump_on = false;
      }
      manual_pump_active = false;  // Clear manual mode

      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_CYAN);
      tft.setTextSize(2);
      tft.setCursor(0, 40);
      tft.println("OTA UPDATE");
      tft.setTextSize(1);
      tft.println("Please wait...");
    });

    ArduinoOTA.onEnd([]() {
      ota_in_progress = false;  // Clear OTA flag
      if (enaDbgSer) Serial.println("\nUpdate complete");
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_GREEN);
      tft.setTextSize(2);
      tft.setCursor(0, 40);
      tft.println("UPDATE OK");
      tft.setTextSize(1);
      tft.println("Rebooting...");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      unsigned int percent = (progress / (total / 100));
      if (enaDbgSer) Serial.printf("Progress: %u%%\r", percent);
      // Update display every 10%
      if (percent % 10 == 0) {
        tft.fillRect(0, 80, 240, 20, ST77XX_BLACK);
        tft.setCursor(0, 80);
        tft.printf("Progress: %u%%", percent);
      }
    });

    ArduinoOTA.onError([](ota_error_t error) {
      ota_in_progress = false;  // Clear OTA flag on error
      if (enaDbgSer) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
      }
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_RED);
      tft.setTextSize(2);
      tft.setCursor(0, 40);
      tft.println("OTA ERROR");
      tft.setTextSize(1);
      if (error == OTA_AUTH_ERROR) {
        tft.println("Auth Failed");
      } else if (error == OTA_BEGIN_ERROR) {
        tft.println("Begin Failed");
      } else if (error == OTA_CONNECT_ERROR) {
        tft.println("Connect Failed");
      } else if (error == OTA_RECEIVE_ERROR) {
        tft.println("Receive Failed");
      } else if (error == OTA_END_ERROR) {
        tft.println("End Failed");
      }
      delay(3000);
    });

    ArduinoOTA.begin();
    if (enaDbgSer) Serial.println("OTA ready");
    debugPrint("OTA: READY", ST77XX_GREEN);
} else {
    if (enaDbgSer) {
      Serial.println();
      Serial.println("WiFi connection failed - continuing without time sync");
    }
    debugPrint("WiFi: FAILED", ST77XX_YELLOW);
    debugPrint("(No time sync)");
    time_synced = false;
  }

  // Load preferences from non-volatile storage
  prefs.begin("pump", false);
  last_start = prefs.getULong("last_start", 0);
  last_dur_sec = prefs.getULong("last_dur_sec", 0);
  last_day_num = prefs.getULong("last_day_num", 0);

  for (int i = 0; i < 5; i++) {
    daily_secs[i] = prefs.getULong(("day" + String(i)).c_str(), 0);
  }
  if (enaDbgSer) Serial.printf("Loaded preferences: last_start=%lu, last_dur=%lu sec\n", last_start, last_dur_sec);

  // Check for new day and rotate daily stats
  if (time_synced) {
    uint32_t today = getDayNumber();
    if (today != last_day_num && last_day_num != 0) {
      if (enaDbgSer) Serial.println("New day detected - rotating daily statistics");
      // Shift array (move yesterday's data to index 1, etc.)
      for (int i = 4; i > 0; i--) {
        daily_secs[i] = daily_secs[i - 1];
        prefs.putULong(("day" + String(i)).c_str(), daily_secs[i]);
      }
      daily_secs[0] = 0;  // Reset today's counter
      prefs.putULong("day0", 0);
    }
    last_day_num = today;
    prefs.putULong("last_day_num", today);
  }

  // Initialize readings buffer
  readings[0] = 0.0;

  // Take initial reading before starting pump control
  debugPrint("");
  debugPrint("Reading sensor...");
  delay(500);
  float level = readWaterLevel();
  readings[0] = level;
  read_idx = 1;
  buffer_full = true;
  current_avg_inches = level;

  char level_buf[32];
  snprintf(level_buf, sizeof(level_buf), "Initial: %.2f in", current_avg_inches);
  debugPrint(level_buf, ST77XX_CYAN);
  if (enaDbgSer) Serial.printf("Initial water level: %.2f inches\n", current_avg_inches);

  // Startup complete
  debugPrint("");
  debugPrint("READY!", ST77XX_GREEN);

  // Set onboard and external pixel 1 to green (identical)
  uint32_t ready_color = pixels.Color(0, 255, 0);
  if (builtin_neo_present) {
    pixels.setPixelColor(0, ready_color);
    pixels.show();
  }
  if (external_neo_present) {
    externalPixel.setPixelColor(0, externalPixel.Color(0, 76, 0));  // Pixel 0: dimmed for now
    externalPixel.setPixelColor(1, ready_color);  // Pixel 1: same as onboard
    externalPixel.show();
  }

  // Boot complete beep
  digitalWrite(BEEPER_PIN, HIGH);
  delay(200);  // 200ms beep
  digitalWrite(BEEPER_PIN, LOW);

  delay(2800);  // Show init screen longer (total 3000ms with beep)
  tft.fillScreen(ST77XX_BLACK);
  if (enaDbgSer) Serial.println("Setup complete - entering main loop\n");
}

void loop() {
  // Handle OTA updates (must be called frequently)
  ArduinoOTA.handle();

  // Read sensor every 2 seconds
  static unsigned long last_read = 0;
  if (millis() - last_read >= 2000) {
    last_read = millis();

    float inches = readWaterLevel();
    readings[0] = inches;
    current_avg_inches = inches;  // No averaging - use current reading directly

    // Debug output every 10 readings (20 seconds)
    static int read_count = 0;
    if (++read_count >= 10) {
      if (enaDbgSer) {
        Serial.println("--- Status Update ---");
        Serial.printf("Water level: %.2f in (current reading)\n", current_avg_inches);
        Serial.printf("Sensor: %s\n", ads_present ? "ADS1115 (real)" : "DUMMY mode");
        Serial.printf("Pump: %s", pump_on ? "ON" : "OFF");
        if (pump_on) {
          Serial.printf(" (runtime: %lu sec)", (millis() - pump_start_ms) / 1000);
        }
        Serial.println();
        Serial.printf("Alarm: %s\n", alarm_active ? "ACTIVE" : "inactive");
        Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
        Serial.println();
      }
      read_count = 0;
    }
  }

  // Button handling with non-blocking edge detection debouncing
  // Skip all button handling during OTA update
  if (!ota_in_progress) {
    static unsigned long last_btn1_press = 0;
    static unsigned long last_btn2_press = 0;
    static bool btn1_was_pressed = false;
    static bool btn2_was_pressed = false;

  // Read both button states
  bool btn1_pressed = (digitalRead(BTN1_PIN) == LOW);
  bool btn2_pressed = (digitalRead(BTN2_PIN) == LOW);

  // Reboot mechanism: Hold BOTH buttons simultaneously for 5 seconds
  // Filter noise: Both buttons must be pressed NOW and in PREVIOUS loop
  // Disable during manual pump mode AND for 2 seconds after any button 1 activity
  bool btn1_recently_used = (millis() - last_btn1_press < 2000);  // 2-second buffer
  if (btn1_pressed && btn2_pressed && btn1_was_pressed && btn2_was_pressed && !manual_pump_active && !btn1_recently_used) {
    // Both buttons confirmed pressed (not noise spike)
    if (!both_buttons_held) {
      // Just started holding both buttons
      both_buttons_held = true;
      both_buttons_hold_start = millis();
      if (enaDbgSer) Serial.println("Both buttons pressed - hold for 5 seconds to reboot");
    } else {
      // Check if held for 5 seconds
      if (millis() - both_buttons_hold_start >= REBOOT_HOLD_TIME_MS && !reboot_triggered) {
        reboot_triggered = true;
        if (enaDbgSer) Serial.println("REBOOT TRIGGERED!");

        // Turn off pump immediately
        if (pump_on) {
          digitalWrite(RELAY_PIN, LOW);
        }

        // Display reboot message
        tft.fillScreen(ST77XX_BLACK);
        tft.setTextColor(ST77XX_RED);
        tft.setTextSize(2);
        tft.setCursor(0, 40);
        tft.println("REBOOTING...");
        tft.setTextSize(1);
        tft.println("Release buttons");

        // Flash all pixels white 5 times
        for (int i = 0; i < 5; i++) {
          if (builtin_neo_present) {
            pixels.setPixelColor(0, pixels.Color(255, 255, 255));
            pixels.show();
          }
          if (external_neo_present) {
            externalPixel.setPixelColor(0, externalPixel.Color(255, 255, 255));
            externalPixel.setPixelColor(1, externalPixel.Color(255, 255, 255));
            externalPixel.show();
          }
          delay(100);
          yield();  // Feed watchdog
          if (builtin_neo_present) {
            pixels.setPixelColor(0, 0);
            pixels.show();
          }
          if (external_neo_present) {
            externalPixel.setPixelColor(0, 0);
            externalPixel.setPixelColor(1, 0);
            externalPixel.show();
          }
          delay(100);
          yield();  // Feed watchdog
        }

        // Wait for buttons to be released to avoid boot issues
        tft.println("Waiting for release...");
        unsigned long release_wait_start = millis();
        const unsigned long RELEASE_TIMEOUT_MS = 10000;  // 10 second timeout

        while (digitalRead(BTN1_PIN) == LOW || digitalRead(BTN2_PIN) == LOW) {
          delay(50);

          // Feed watchdog to prevent reset during button release wait
          yield();  // Allows background tasks and watchdog to run

          // Timeout safety: If user doesn't release after 10 seconds, reboot anyway
          if (millis() - release_wait_start > RELEASE_TIMEOUT_MS) {
            if (enaDbgSer) Serial.println("Button release timeout - rebooting anyway");
            break;
          }
        }

        // Perform reboot
        ESP.restart();
      }
    }
  } else {
    // One or both buttons released - reset hold timer
    both_buttons_held = false;
    reboot_triggered = false;
  }

  // Skip normal button handling if both buttons are held (reboot in progress)
  if (!both_buttons_held) {

  // Button 1: Manual pump control with timed mode (edge detection - no blocking)

  // Detect press start (rising edge)
  if (btn1_pressed && !btn1_was_pressed && (millis() - last_btn1_press > 500)) {
    last_btn1_press = millis();
    btn1_press_start_ms = millis();  // Record when button was pressed
  }

  // Detect press end (falling edge) - button released
  if (!btn1_pressed && btn1_was_pressed) {
    unsigned long press_duration = millis() - btn1_press_start_ms;

    // Long press (4+ seconds) - turn pump OFF if running
    if (press_duration >= MANUAL_LONG_PRESS_MS) {
      if (pump_on && manual_pump_active) {
        if (enaDbgSer) Serial.println("Button 1 long press - stopping manual pump");
        stopPump();
        manual_pump_active = false;
        manual_press_count = 0;
      }
    } else {
      // Short press - handle based on manual mode state
      if (!manual_pump_active) {
        // First press - start manual mode
        manual_pump_active = true;
        manual_press_count = 1;
        manual_press_window_start_ms = millis();
        manual_pump_end_ms = millis() + MANUAL_TIME_PER_PRESS_MS;  // 20 seconds
        if (enaDbgSer) Serial.println("Button 1: Manual pump ON - 20 seconds");
        startPump();
      } else {
        // Manual mode already active
        unsigned long since_first_press = millis() - manual_press_window_start_ms;

        if (since_first_press <= MANUAL_PRESS_WINDOW_MS && manual_press_count < MANUAL_MAX_PRESSES) {
          // Within 10-second window and haven't reached max presses - add 20 seconds
          manual_press_count++;
          manual_pump_end_ms += MANUAL_TIME_PER_PRESS_MS;
          unsigned long remaining = (manual_pump_end_ms - millis()) / 1000;
          if (enaDbgSer) Serial.printf("Button 1: Added 20 seconds (press %d/%d, total: %lu sec)\n",
                        manual_press_count, MANUAL_MAX_PRESSES, remaining);
        } else {
          // After 10-second window OR max presses reached - turn OFF
          if (enaDbgSer) Serial.println("Button 1: Stopping manual pump");
          stopPump();
          manual_pump_active = false;
          manual_press_count = 0;
        }
      }
    }
  }

  btn1_was_pressed = btn1_pressed;

  // Button 2: Alarm reset + Statistics reset mechanism (edge detection - no blocking)
  // Use btn2_pressed already read at line 565 (don't re-read to avoid race condition)
  if (btn2_pressed && !btn2_was_pressed && (millis() - last_btn2_press > 500)) {
    last_btn2_press = millis();

    // Statistics reset mechanism: 10 presses within 30 seconds
    if (btn2_reset_count == 0) {
      // Start new reset sequence
      btn2_reset_start = millis();
      btn2_reset_count = 1;
      if (enaDbgSer) Serial.println("Button 2 press 1/10 for stats reset");
    } else {
      // Check if still within timeout window
      if (millis() - btn2_reset_start < RESET_TIMEOUT_MS) {
        btn2_reset_count++;
        if (enaDbgSer) Serial.printf("Button 2 press %d/10 for stats reset\n", btn2_reset_count);

        if (btn2_reset_count >= RESET_PRESS_COUNT) {
          // Reset all statistics!
          if (enaDbgSer) Serial.println("Resetting all statistics!");
          last_start = 0;
          last_dur_sec = 0;
          for (int i = 0; i < 5; i++) {
            daily_secs[i] = 0;
            prefs.putULong(("day" + String(i)).c_str(), 0);
          }
          prefs.putULong("last_start", 0);
          prefs.putULong("last_dur_sec", 0);

          // Visual feedback: flash all pixels white 3 times
          for (int i = 0; i < 3; i++) {
            if (builtin_neo_present) {
              pixels.setPixelColor(0, pixels.Color(255, 255, 255));
              pixels.show();
            }
            if (external_neo_present) {
              externalPixel.setPixelColor(0, externalPixel.Color(255, 255, 255));
              externalPixel.setPixelColor(1, externalPixel.Color(255, 255, 255));
              externalPixel.show();
            }
            delay(200);
            if (builtin_neo_present) {
              pixels.setPixelColor(0, 0);
              pixels.show();
            }
            if (external_neo_present) {
              externalPixel.setPixelColor(0, 0);
              externalPixel.setPixelColor(1, 0);
              externalPixel.show();
            }
            delay(200);
          }

          // Reset counter
          btn2_reset_count = 0;
        }
      } else {
        // Timeout expired, restart sequence
        btn2_reset_start = millis();
        btn2_reset_count = 1;
        if (enaDbgSer) Serial.println("Button 2 reset timeout, restarting sequence");
      }
    }

    // Also handle alarm reset if alarm is active
    if (alarm_active) {
      if (enaDbgSer) Serial.println("Button 2 pressed - resetting alarm");
      alarm_active = false;

      // Set onboard and external pixel 1 to green (identical)
      uint32_t green = pixels.Color(0, 255, 0);
      if (builtin_neo_present) {
        pixels.setPixelColor(0, green);
        pixels.show();
      }
      if (external_neo_present) {
        // Pixel 0 will be handled by water level indicator
        externalPixel.setPixelColor(1, green);  // Pixel 1: same as onboard
        externalPixel.show();
      }
      digitalWrite(BEEPER_PIN, LOW);
    }
  }
  btn2_was_pressed = btn2_pressed;

  // Reset button 2 counter if timeout expires
  if (btn2_reset_count > 0 && (millis() - btn2_reset_start > RESET_TIMEOUT_MS)) {
    btn2_reset_count = 0;
    if (enaDbgSer) Serial.println("Button 2 reset sequence expired");
  }

  }  // End of !both_buttons_held block (normal button handling)
  }  // End of !ota_in_progress block for button handling

  // Manual pump timer check (skip during OTA)
  if (!ota_in_progress && manual_pump_active) {
    // Use signed math to handle millis() overflow correctly
    if ((long)(millis() - manual_pump_end_ms) >= 0) {
      if (enaDbgSer) Serial.println("Manual pump timer expired - stopping pump");
      stopPump();
      manual_pump_active = false;
      manual_press_count = 0;
    }
  }

  // Daily clock resync at 6:45 AM
  if (time_synced && WiFi.status() == WL_CONNECTED) {
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    uint32_t today = getDayNumber();

    // If it's 6:45 AM and we haven't resynced today
    if (tm->tm_hour == CLOCK_SYNC_HOUR && tm->tm_min == CLOCK_SYNC_MINUTE && last_clock_sync_day != today) {
      if (enaDbgSer) Serial.println("Resyncing clock with NTP server...");
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      delay(2000);  // Allow time for NTP sync
      last_clock_sync_day = today;
      if (enaDbgSer) Serial.println("Clock resync complete");
    }
  }

  // NTP retry logic if time not synced
  if (!time_synced) {
    unsigned long time_since_boot = millis() - boot_time_ms;
    unsigned long time_since_last_retry = millis() - last_ntp_retry_ms;

    // Determine retry interval based on time since boot
    unsigned long retry_interval;
    if (time_since_boot < NTP_EARLY_PERIOD_MS) {
      // First 10 minutes: retry every 2 minutes
      retry_interval = NTP_RETRY_INTERVAL_EARLY_MS;
    } else {
      // After 10 minutes: retry every 15 minutes
      retry_interval = NTP_RETRY_INTERVAL_LATE_MS;
    }

    // Check if it's time to retry
    if (last_ntp_retry_ms == 0 || time_since_last_retry >= retry_interval) {
      last_ntp_retry_ms = millis();
      if (enaDbgSer) {
        Serial.printf("NTP retry attempt (%.1f min since boot, interval: %lu min)\n",
                     time_since_boot / 60000.0,
                     retry_interval / 60000);
      }

      // Attempt NTP sync (this handles WiFi reconnection if needed)
      if (attemptNTPSync()) {
        if (enaDbgSer) Serial.println("NTP sync successful!");
        // time_synced is set to true in attemptNTPSync()
        // Display will update on next refresh cycle
      }
    }
  }

  // Daily maintenance cycle at 7:00 AM (not logged in statistics)
  if (time_synced && !maintenance_mode) {
    // Check if it's 7:00 AM and we haven't run today
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    uint32_t today = getDayNumber();

    // If it's 7:00 AM hour and we haven't run maintenance today
    if (tm->tm_hour == MAINTENANCE_HOUR && last_maintenance_day != today) {
      // Check water level
      if (current_avg_inches >= MAINTENANCE_TRIGGER_INCHES) {
        // Start maintenance cycle
        maintenance_mode = true;
        maintenance_start_ms = millis();
        last_maintenance_day = today;
        if (enaDbgSer) Serial.printf("Starting 7AM maintenance cycle - water level: %.2f inches\n", current_avg_inches);
        startPump();
      } else {
        // Water level too low, skip but mark as done for today
        last_maintenance_day = today;
        if (enaDbgSer) Serial.printf("Skipping 7AM maintenance - water level %.2f < %.2f inches\n",
                      current_avg_inches, MAINTENANCE_TRIGGER_INCHES);
      }
    }
  }

  // Handle active maintenance cycle
  if (maintenance_mode) {
    unsigned long elapsed_sec = (millis() - maintenance_start_ms) / 1000;

    if (maintenance_extension_active) {
      // Handle 3-second extension after reaching target level
      unsigned long extension_elapsed = (millis() - maintenance_extension_start_ms) / 1000;
      if (extension_elapsed >= MAINTENANCE_EXTENSION_SEC) {
        if (enaDbgSer) Serial.printf("Maintenance extension complete - stopping pump (total runtime: %lu sec)\n", elapsed_sec);
        stopPump();
        maintenance_mode = false;
        maintenance_extension_active = false;
      } else if (elapsed_sec >= MAINTENANCE_MAX_SEC) {
        // Safety: Stop if total time exceeds max (even during extension)
        if (enaDbgSer) Serial.printf("Maintenance max time reached during extension (%lu sec)\n", elapsed_sec);
        stopPump();
        maintenance_mode = false;
        maintenance_extension_active = false;
      }
    } else {
      // Not in extension yet - check if target reached
      if (current_avg_inches <= MAINTENANCE_TARGET_INCHES) {
        // Start 3-second extension
        if (enaDbgSer) Serial.printf("Maintenance target reached (%.2f <= %.2f inches) - starting 3-second extension\n",
                      current_avg_inches, MAINTENANCE_TARGET_INCHES);
        maintenance_extension_active = true;
        maintenance_extension_start_ms = millis();
      } else if (elapsed_sec >= MAINTENANCE_MAX_SEC) {
        // Max time reached before hitting target - stop without extension
        if (enaDbgSer) Serial.printf("Maintenance cycle max time reached (%lu sec) - water level: %.2f inches\n",
                      MAINTENANCE_MAX_SEC, current_avg_inches);
        stopPump();
        maintenance_mode = false;
      }
    }
  }

  // Automatic pump control logic with pause/restart and extension
  // Skip normal pump control if in maintenance mode or manual mode
  if (!maintenance_mode && !manual_pump_active) {
    if (pump_in_pause) {
      // Check if pause period is over
      if (millis() - pump_pause_start_ms >= (PAUSE_SEC * 1000UL)) {
        if (enaDbgSer) Serial.println("Pause complete - restarting pump if still needed");
        pump_in_pause = false;
        // Will restart pump on next loop if water still high
      }
    } else if (pump_extension_active) {
      // Handle 20-second extension after reaching OFF_INCHES
      if (millis() - pump_extension_start_ms >= (EXTENSION_SEC * 1000UL)) {
        if (enaDbgSer) Serial.println("Extension complete - stopping pump");
        stopPump();
        pump_extension_active = false;
      }
    } else if (!pump_on && current_avg_inches >= TRIGGER_INCHES) {
      // Start pump when water reaches trigger level
      if (enaDbgSer) Serial.printf("Water level %.2f >= %.2f - starting pump\n", current_avg_inches, TRIGGER_INCHES);
      startPump();

    } else if (pump_on && current_avg_inches <= OFF_INCHES && !pump_extension_active) {
      // When reaching OFF_INCHES, start 20-second extension
      if (enaDbgSer) Serial.printf("Water level %.2f <= %.2f - starting 20-second extension\n", current_avg_inches, OFF_INCHES);
      pump_extension_active = true;
      pump_extension_start_ms = millis();

    } else if (pump_on && (millis() - pump_start_ms) >= (TIMEOUT_SEC * 1000UL)) {
      // Pump has run for 4 minutes
      if (current_avg_inches > OFF_INCHES) {
        if (enaDbgSer) Serial.printf("Pump timeout after %d sec, water still at %.2f in - pausing for %d sec\n",
                      TIMEOUT_SEC, current_avg_inches, PAUSE_SEC);
        stopPump();
        pump_in_pause = true;
        pump_pause_start_ms = millis();
      }
    }
  }  // End of normal pump control (if !maintenance_mode)

  // High water alarm at 10 inches
  if (current_avg_inches > 10.0) {
    if (!alarm_active) {
      if (enaDbgSer) Serial.printf("HIGH WATER ALARM! Level: %.2f inches\n", current_avg_inches);
      alarm_active = true;
    }
  } else {
    // Clear alarm if water level drops below 10 inches
    if (alarm_active && current_avg_inches <= 9.5) {  // Hysteresis: clear at 9.5"
      if (enaDbgSer) Serial.println("Water level dropped - clearing alarm");
      alarm_active = false;
    }
  }

  // Alarm handling
  if (alarm_active) {
    handleAlarm();
  } else {
    digitalWrite(BEEPER_PIN, LOW);

    // Normal NeoPixel indication for onboard and external pixel 1 (identical)
    // Green: low level (< 6"), Blue: medium-high level (6-8"), Purple: very high (>8")
    uint32_t status_color;
    if (current_avg_inches < 6.0) {
      status_color = pixels.Color(0, 255, 0);  // Green - normal
    } else if (current_avg_inches < 8.0) {
      status_color = pixels.Color(0, 0, 255);  // Blue - getting high
    } else {
      status_color = pixels.Color(128, 0, 128);  // Purple - very high (pump should be running)
    }

    // Soft blink (500ms on, 500ms off) for onboard and external pixel 1
    bool blink_on = (millis() % 1000 < 500);
    uint32_t display_color = blink_on ? status_color : 0;

    if (builtin_neo_present) {
      pixels.setPixelColor(0, display_color);
      pixels.show();
    }

    if (external_neo_present) {
      // External pixel 1: Same as onboard (identical behavior)
      externalPixel.setPixelColor(1, display_color);

      // External pixel 0: Water level indicator (special behavior)
      uint32_t pixel0_color = 0;

      // Find matching water level range in table
      const WaterLevelIndicator* level_config = NULL;
      for (uint8_t i = 0; i < WATER_LEVEL_TABLE_SIZE; i++) {
        if (current_avg_inches >= WATER_LEVEL_TABLE[i].min_inches &&
            current_avg_inches < WATER_LEVEL_TABLE[i].max_inches) {
          level_config = &WATER_LEVEL_TABLE[i];
          break;
        }
      }

      if (level_config != NULL) {
        if (level_config->flash_count == 0) {
          // Below 1 inch: Use same color as status (but dimmed to 30%)
          if (blink_on) {
            uint8_t r = (status_color >> 16) & 0xFF;
            uint8_t g = (status_color >> 8) & 0xFF;
            uint8_t b = status_color & 0xFF;
            pixel0_color = externalPixel.Color(r * 30 / 100, g * 30 / 100, b * 30 / 100);
          }
        } else {
          // Above 1 inch: Flash N times at specified Hz, then 2 seconds OFF, repeat
          // Calculate timing:
          // - Each blink period = 1000ms / flash_hz (e.g., 2Hz = 500ms per blink)
          // - Flash duration = flash_count * (1000ms / flash_hz)
          // - Pause duration = 2000ms
          // - Total cycle = flash_duration + pause_duration

          uint16_t blink_period_ms = 1000 / level_config->flash_hz;  // Time for one on/off cycle
          uint16_t flash_duration_ms = level_config->flash_count * blink_period_ms;  // Total flash time
          uint16_t pause_duration_ms = 2000;  // 2 seconds OFF between sequences
          uint16_t cycle_period_ms = flash_duration_ms + pause_duration_ms;

          unsigned long cycle_pos = millis() % cycle_period_ms;

          if (cycle_pos < flash_duration_ms) {
            // Within the flash period: determine which blink we're in
            uint16_t blink_number = cycle_pos / blink_period_ms;  // Which blink (0 to flash_count-1)
            uint16_t pos_in_blink = cycle_pos % blink_period_ms;  // Position within current blink

            // On for first half of blink period, off for second half
            bool flash_on = (pos_in_blink < (blink_period_ms / 2));

            if (flash_on && blink_number < level_config->flash_count) {
              // Apply brightness scaling to RGB values
              uint8_t r = level_config->red * level_config->brightness / 100;
              uint8_t g = level_config->green * level_config->brightness / 100;
              uint8_t b = level_config->blue * level_config->brightness / 100;
              pixel0_color = externalPixel.Color(r, g, b);
            }
          }
          // else: in pause period, pixel0_color stays 0 (OFF)
        }
      }

      externalPixel.setPixelColor(0, pixel0_color);
      externalPixel.show();
    }
  }

  // Hourly save of statistics to flash (to prevent loss on power failure)
  if (millis() - last_hourly_save >= 3600000UL) {  // 3600000ms = 1 hour
    last_hourly_save = millis();
    if (enaDbgSer) Serial.println("Performing hourly save of statistics to flash");
    prefs.putULong("last_start", last_start);
    prefs.putULong("last_dur_sec", last_dur_sec);
    prefs.putULong("last_day_num", last_day_num);
    for (int i = 0; i < 5; i++) {
      prefs.putULong(("day" + String(i)).c_str(), daily_secs[i]);
    }
  }

  // Update display every second
  if (millis() - last_display_update >= DISPLAY_INTERVAL) {
    last_display_update = millis();
    updateDisplay();
  }

  delay(100);  // Main loop throttle
}

/**
 * Read water level from sensor via ADS1115
 * Returns: Water level in inches (0-18 range)
 * If ADS1115 is not present, returns simulated dummy values
 */
float readWaterLevel() {
  // If ADS1115 is not present, return dummy/simulated values
  if (!ads_present) {
    // Simulate realistic water level with slow variation
    static float dummy_level = 2.5;  // Start at 2.5 inches
    static unsigned long last_change = 0;
    static float target_level = 4.0;

    // Change target every 30 seconds
    if (millis() - last_change > 30000) {
      target_level = random(100, 700) / 100.0;  // Random 1.0 to 7.0 inches
      last_change = millis();
      if (enaDbgSer) Serial.printf("[DUMMY] New target level: %.2f inches\n", target_level);
    }

    // Slowly move toward target (0.05 inches per reading)
    if (dummy_level < target_level) {
      dummy_level += 0.05;
    } else if (dummy_level > target_level) {
      dummy_level -= 0.05;
    }

    // Add small random noise
    float noise = (random(-20, 20)) / 100.0;  // ±0.2 inch noise
    float result = dummy_level + noise;

    // Clamp to valid range
    if (result < 0.0) result = 0.0;
    if (result > 18.0) result = 18.0;

    // Add sensor offset (sensor mounted above actual bottom)
    return result + SENSOR_OFFSET_INCHES;
  }

  // Real ADS1115 reading
  int16_t adc = ads.readADC_SingleEnded(0);
  float vout = ads.computeVolts(adc);  // Voltage after divider (0-3.33V)

  // Undo voltage divider: 10kΩ/(20kΩ+10kΩ) = 0.333
  // So: Vin = Vout / 0.333 = Vout * 3.0
  float vin = vout * 3.0;

  // Convert voltage to inches: 10V = 1m = 39.37 inches
  float inches = (vin / 10.0) * 39.37;

  // Clamp to sump maximum depth
  if (inches > 18.0) inches = 18.0;
  if (inches < 0.0) inches = 0.0;

  // Add sensor offset (sensor mounted above actual bottom)
  return inches + SENSOR_OFFSET_INCHES;
}

/**
 * Calculate average of readings buffer
 */
float calculateAverage() {
  float sum = 0.0;
  int count = buffer_full ? 20 : read_idx;

  if (count == 0) return 0.0;

  for (int i = 0; i < count; i++) {
    sum += readings[i];
  }

  return sum / count;
}

/**
 * Start the pump
 */
void startPump() {
  if (!pump_on) {
    pump_on = true;
    digitalWrite(RELAY_PIN, HIGH);  // Turn relay ON (flipped logic)
    pump_start_ms = millis();

    // Only log timestamp if NOT in maintenance mode
    if (!maintenance_mode && time_synced) {
      time_t now;
      time(&now);
      last_start = now;
      if (enaDbgSer) Serial.printf("Pump started at %lu\n", last_start);
    } else if (maintenance_mode) {
      if (enaDbgSer) Serial.println("Pump started (maintenance mode - not logged)");
    } else {
      if (enaDbgSer) Serial.println("Pump started (no timestamp - WiFi not connected)");
    }
  }
}

/**
 * Stop the pump and save statistics
 */
void stopPump() {
  if (pump_on) {
    // Calculate duration BEFORE turning off relay (in case EMI causes reset)
    unsigned long dur_ms = millis() - pump_start_ms;
    unsigned long dur_sec = dur_ms / 1000;

    // Save statistics to flash BEFORE turning off relay (protect against EMI reset)
    // Only save statistics if NOT in maintenance mode
    if (!maintenance_mode) {
      last_dur_sec = dur_sec;

      // Save to preferences (pump still running)
      prefs.putULong("last_dur_sec", last_dur_sec);

      // Add to today's total (in seconds for mm:ss display)
      daily_secs[0] += last_dur_sec;
      prefs.putULong("day0", daily_secs[0]);

      // Store start time
      prefs.putULong("last_start", last_start);

      if (enaDbgSer) Serial.printf("Pump stopping after %lu seconds (%lu min) - stats saved\n",
                    last_dur_sec, last_dur_sec / 60);

      // Wait for flash write to complete before turning off relay (prevent data loss)
      delay(50);  // 50ms should be sufficient for NVS write
    } else {
      if (enaDbgSer) Serial.printf("Pump stopping after %lu seconds (maintenance mode - not logged)\n", dur_sec);
    }

    // NOW turn off pump (statistics already saved)
    pump_on = false;
    digitalWrite(RELAY_PIN, LOW);  // Turn relay OFF - may cause EMI reset, but data is safe
  }
}

/**
 * Handle alarm state - flash LED and beep
 */
void handleAlarm() {
  // Beeper: 2 second cycle, 25% duty (500ms on, 1500ms off)
  unsigned long cycle_pos = millis() % 2000;
  digitalWrite(BEEPER_PIN, (cycle_pos < 500) ? HIGH : LOW);

  // NeoPixels: Flash red at 1Hz for onboard and external pixel 1 (identical)
  uint32_t red_color = (millis() % 1000 < 500) ? pixels.Color(255, 0, 0) : pixels.Color(0, 0, 0);

  if (builtin_neo_present) {
    pixels.setPixelColor(0, red_color);
    pixels.show();
  }

  if (external_neo_present) {
    // External pixel 1: Same as onboard (identical behavior)
    externalPixel.setPixelColor(1, red_color);

    // External pixel 0: Continue showing water level indicator during alarm
    uint32_t pixel0_color = 0;

    // Find matching water level range in table
    const WaterLevelIndicator* level_config = NULL;
    for (uint8_t i = 0; i < WATER_LEVEL_TABLE_SIZE; i++) {
      if (current_avg_inches >= WATER_LEVEL_TABLE[i].min_inches &&
          current_avg_inches < WATER_LEVEL_TABLE[i].max_inches) {
        level_config = &WATER_LEVEL_TABLE[i];
        break;
      }
    }

    if (level_config != NULL && level_config->flash_count > 0) {
      // Flash N times at specified Hz, then 2 seconds OFF, repeat
      uint16_t blink_period_ms = 1000 / level_config->flash_hz;
      uint16_t flash_duration_ms = level_config->flash_count * blink_period_ms;
      uint16_t pause_duration_ms = 2000;  // 2 seconds OFF
      uint16_t cycle_period_ms = flash_duration_ms + pause_duration_ms;

      unsigned long cycle_pos = millis() % cycle_period_ms;

      if (cycle_pos < flash_duration_ms) {
        // Within the flash period: determine which blink we're in
        uint16_t blink_number = cycle_pos / blink_period_ms;
        uint16_t pos_in_blink = cycle_pos % blink_period_ms;

        // On for first half of blink period, off for second half
        bool flash_on = (pos_in_blink < (blink_period_ms / 2));

        if (flash_on && blink_number < level_config->flash_count) {
          uint8_t r = level_config->red * level_config->brightness / 100;
          uint8_t g = level_config->green * level_config->brightness / 100;
          uint8_t b = level_config->blue * level_config->brightness / 100;
          pixel0_color = externalPixel.Color(r, g, b);
        }
      }
      // else: in pause period, pixel0_color stays 0 (OFF)
    }

    externalPixel.setPixelColor(0, pixel0_color);
    externalPixel.show();
  }
}

/**
 * Get unique day number for tracking daily statistics
 * Format: YYYYDDD (year + day of year)
 */
uint32_t getDayNumber() {
  time_t now;
  time(&now);
  struct tm *tm = localtime(&now);
  int doy = tm->tm_yday + 1;  // Day of year (1-366)
  int year = tm->tm_year + 1900;
  return (year * 1000UL) + doy;
}

/**
 * Update TFT display with current status
 */
void updateDisplay() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  int y = 0;

  // Current water level (large, prominent)
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(0, y);
  tft.printf("Level: %.1f in", current_avg_inches);

  // Clock display (upper right, yellow, size 1)
  if (time_synced) {
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);

    // Convert to 12-hour format without leading zero
    int hour12 = tm->tm_hour % 12;
    if (hour12 == 0) hour12 = 12;  // Midnight/noon = 12

    char ampm = (tm->tm_hour < 12) ? 'A' : 'P';

    char time_buf[16];
    sprintf(time_buf, "%2d:%02d:%02d%c", hour12, tm->tm_min, tm->tm_sec, ampm);

    tft.setTextSize(1);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(186, y);  // Right-justify (240 - 54 pixels)
    tft.print(time_buf);
  }

  // Firmware version (right-justified, white, size 1) just under the clock.
  // Drawn whether or not NTP has synced, so it is always visible.
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  const char* ver_text = "ver " FW_VERSION;
  tft.setCursor(240 - (int)strlen(ver_text) * 6, 10);  // 6 px per character at size 1
  tft.print(ver_text);

  y += 25;

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  // Last pump run information
  if (time_synced && last_start > 0 && last_dur_sec > 0) {
    time_t start_time = (time_t)last_start;  // Proper conversion
    struct tm *tm = localtime(&start_time);
    char buf[64];
    strftime(buf, sizeof(buf), "%m/%d/%y %I:%M%p", tm);  // 11/04/25 11:48PM format

    tft.setCursor(0, y);
    // Format duration as sec or min
    if (last_dur_sec < 60) {
      tft.printf("Last: %s  %lu sec", buf, last_dur_sec);
    } else {
      tft.printf("Last: %s  %lu min", buf, last_dur_sec / 60);
    }
    y += 15;
  } else if (!time_synced) {
    tft.setCursor(0, y);
    tft.println("Last run: (no time)");
    y += 15;
  } else {
    // time_synced but no previous run yet
    tft.setCursor(0, y);
    tft.println("Last run: (none today)");
    y += 15;
  }

  // Totals: Today and 5-day runtime in mm:ss format
  uint32_t total_5day_secs = 0;
  for (int i = 0; i < 5; i++) {
    total_5day_secs += daily_secs[i];
  }

  // Convert to mm:ss format
  uint32_t today_mins = daily_secs[0] / 60;
  uint32_t today_secs = daily_secs[0] % 60;
  uint32_t total_mins = total_5day_secs / 60;
  uint32_t total_secs = total_5day_secs % 60;

  tft.setCursor(0, y);
  tft.printf("Totals (mm:ss) Today: %lu:%02lu, 5day: %lu:%02lu", today_mins, today_secs, total_mins, total_secs);
  y += 20;

  // Current status (color-coded)
  String status;
  uint16_t status_color;

  if (alarm_active) {
    status = "ALARM!";
    status_color = ST77XX_RED;
  } else if (pump_on) {
    unsigned long runtime_sec = (millis() - pump_start_ms) / 1000;
    status = "PUMP ON (" + String(runtime_sec) + "s)";
    status_color = ST77XX_YELLOW;
  } else {
    status = "WATCHING";
    status_color = ST77XX_GREEN;
  }

  tft.setTextSize(2);
  tft.setTextColor(status_color);
  tft.setCursor(0, y);
  tft.print(status);
  y += 25;

  // Manual pump mode countdown display (red countdown) - only show when active
  if (manual_pump_active && pump_on) {
    // Use signed math to handle millis() overflow and prevent underflow
    long remaining_ms = (long)(manual_pump_end_ms - millis());
    if (remaining_ms > 0) {  // Only display if timer hasn't expired yet
      unsigned long remaining_sec = remaining_ms / 1000;
      tft.setTextSize(2);
      tft.setTextColor(ST77XX_RED);
      tft.setCursor(0, y);

      // Check if we're still in the 10-second window for adding time
      bool in_window = (millis() - manual_press_window_start_ms) <= MANUAL_PRESS_WINDOW_MS;

      // Show "  ." indicator during window, hide after window expires
      if (in_window) {
        tft.printf("PUMP ON (%lu sec) .", remaining_sec);
      } else {
        tft.printf("PUMP ON (%lu sec)", remaining_sec);
      }
      y += 25;
    }
  }

  // Relay states, beeper, and misc inputs (moved down to just above sensor status)
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(0, 110);

  // Read GPIO pin states (HIGH=1, LOW=0)
  int gpio14_state = digitalRead(MISC_INPUT1_PIN);
  int gpio8_state = digitalRead(MISC_INPUT2_PIN);

  tft.printf("K1:%s K2:%s Problem?:%s IO14:%d IO8:%d",
             relay1_on ? "ON" : "OFF",
             relay2_on ? "ON" : "OFF",
             alarm_active ? "YES" : "no",
             gpio14_state,
             gpio8_state);

  // Show sensor status
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(0, 120);
  if (ads_present) {
    tft.setTextColor(ST77XX_GREEN);
    tft.print("ADC:OK");
  } else {
    tft.setTextColor(ST77XX_YELLOW);
    tft.print("ADC:DUMMY");
  }

  // Show WiFi status
  tft.setCursor(80, 120);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(ST77XX_GREEN);
    int rssi = WiFi.RSSI();
    tft.printf("WiFi:%ddBm %s", rssi, WiFi.localIP().toString().c_str());
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.print("WiFi:--");
  }
}
