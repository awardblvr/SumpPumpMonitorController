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
#include "sump_logic.h"         // Pure logic: level validation, command parsing, report scheduling (unit-tested on a PC)

// Firmware version - shown on the idle display (under the clock) and on the boot screen.
// Bump this every time you upload a new build so you can tell at a glance what is running.
// 2.0 = MQTT / Home Assistant interface (see SumpMqtt.ino), remote commands, pump disable, editable levels.
// 2.1 = display only: MQTT status indicator moved down to y=20. MQTT interface unchanged (still interface 2.0).
#define FW_VERSION "2.1"

// Bench-test only: when 1, a missing ADS1115 is replaced by RANDOM simulated water levels.
// Leave at 0 for the installed controller - simulated levels must never drive a real pump.
#define ENABLE_DUMMY_SENSOR 0

// 1 = MQTT / Home Assistant interface (SumpMqtt.ino). 0 = build without it (stubs), e.g. to roll back to a
// standalone controller.
#define ENABLE_MQTT 1

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

// WiFi and Time (US Pacific, automatic DST - see TZ_INFO below)
// Credentials live in secrets.h (git-ignored). Copy secrets.h.example to secrets.h and edit.
const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASSWORD;
// POSIX timezone string: US Pacific, switches PST<->PDT automatically (2nd Sun Mar / 1st Sun Nov).
// (v1.0 used a fixed UTC-8 offset, so the clock and the 7:00 AM cycle were an hour off in summer.)
const char* TZ_INFO = "PST8PDT,M3.2.0,M11.1.0";
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
// Level thresholds are RUNTIME parameters (trigger_inches / off_inches / alarm_inches below). The values
// here are the factory defaults, used until changed with the MQTT "set" command, which saves any change to flash.
#define DEFAULT_TRIGGER_INCHES 5.0
#define DEFAULT_OFF_INCHES 1.0
#define DEFAULT_ALARM_INCHES 10.0      // High-water alarm above this; clears 0.5 in lower (hysteresis)
#define SENSOR_OFFSET_INCHES 0.0  // Sensor offset (was 1.5")
#define TIMEOUT_SEC 240  // 4 minutes max runtime
#define PAUSE_SEC 60     // 1 minute pause after timeout
#define EXTENSION_SEC 5 // 5 second extension after reaching the OFF level

// Alarm thresholds: HIGH_WATER_CLEAR is alarm_inches - ALARM_HYSTERESIS_INCHES (alarm_inches is a runtime parameter)
#define ALARM_HYSTERESIS_INCHES 0.5
const unsigned long ALARM_RESOUND_MS = 30UL * 60UL * 1000UL;  // A silenced alarm sounds again after 30 min if still active

// Remote-command limits (MQTT)
#define FORCE_RUN_DEFAULT_SEC SUMP_FORCE_MAX_SEC          // "pump_on": forced run, never longer than 5 minutes
#define DISABLE_DEFAULT_HOURS SUMP_DISABLE_MAX_HOURS      // "disable": 24 hours unless a shorter time is given
// 1 = a command-disabled pump re-enables itself the moment the high-water alarm is active (level above alarm_inches).
// A stray or mistaken "disable" must not leave a flooding sump unpumped for up to 24 hours when nobody is there to
// press "enable". 0 = a disable always runs its full time (only the beeper, display and MQTT then react to high water).
#define DISABLE_CANCELS_AT_ALARM 1
const unsigned long CMD_MESSAGE_MS = 6000;                // How long a command response stays on the screen

// Reporting schedule (see SumpReporter in sump_logic.h)
#define REPORT_BASELINE_MS (5UL * 60UL * 1000UL)          // normal: one report every 5 minutes (plus one on any event)
#define REPORT_ELEVATED_MS (2UL * 60UL * 1000UL)          // water rising / problem: every 2 minutes
#define REPORT_ELEVATED_MAX_MS (6UL * 60UL * 60UL * 1000UL)  // elevated reporting ends by itself after 6 hours
#define REPORT_CALM_MS (5UL * 60UL * 1000UL)              // ...or after this long with everything back to normal
#define REPORT_SAMPLE_MS 30000UL                          // level history spacing (20 samples = 10 minutes)
#define REPORT_RISE_INCHES 0.5f                           // "water is rising" = up this much within 10 minutes
#define REPORT_NORMAL_INCHES 2.0f                         // "back to normal" needs the level at or below this

// Pump trouble: this many back-to-back 4-minute timeouts (water still high) latches PUMP TROUBLE
#define PUMP_TROUBLE_TIMEOUTS 3

// Sensor supervision
#define ADS_I2C_ADDR 0x48                       // ADS1115 address (ADDR pin to GND)
#define LEVEL_SAMPLES 5                         // Median of this many conversions per reading (rejects EMI spikes)
#define ADS_CONVERSION_TIMEOUT_MS 30            // One conversion takes ~8 ms at 128 SPS
#define SENSOR_FAULT_READS 5                    // Consecutive failed reads (2 s apart) before SENSOR FAULT
#define SENSOR_OK_READS 3                       // Consecutive good reads before the fault clears
// While the level sensor is faulted the displayed level is frozen at the last good value. 1 = automatic
// control will not START the pump from that stale value (a run already in progress still ends at the
// 240 s timeout and is not restarted, so a pump can run dry for at most one timeout window). Manual
// button runs still work and the alarm sounds. 0 = keep acting on the frozen level, which can run a pump
// dry on and off indefinitely, or never start one in a flood - this is a policy call, see the analysis doc.
#define SENSOR_FAULT_BLOCKS_AUTO_START 1
const unsigned long ADS_REINIT_INTERVAL_MS = 10000;  // Retry ads.begin() this often while it is missing

// Network supervision (all non-blocking - the pump control loop never waits on WiFi/NTP)
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;   // Re-issue a WiFi connect this often while disconnected
const unsigned long NTP_RESTART_INTERVAL_MS = 120000; // Re-issue the NTP request if still unsynced after this long

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
bool alarm_active = false;       // Any alarm condition below (drives beeper / red LEDs / display)
bool high_water_alarm = false;   // Level above alarm_inches
bool pump_trouble = false;       // PUMP_TROUBLE_TIMEOUTS back-to-back timeouts with water still high
bool sensor_fault = false;       // Level sensor not answering - level shown is the last good value
uint8_t pump_consecutive_timeouts = 0;
bool alarm_silenced = false;     // Button 2 / "silence" command: beeper muted, LEDs/display still show the alarm
unsigned long alarm_silenced_ms = 0;

// Level thresholds - runtime parameters, loaded from flash at boot, changed by the MQTT "set" command
float trigger_inches = DEFAULT_TRIGGER_INCHES;   // automatic pump starts at or above this
float off_inches = DEFAULT_OFF_INCHES;           // pump begins its stop extension at or below this
float alarm_inches = DEFAULT_ALARM_INCHES;       // high-water alarm above this (clears ALARM_HYSTERESIS_INCHES lower)

// Remote control state (MQTT commands)
bool pump_disabled = false;                      // "disable": no automatic pumping until pump_disabled_until_ms
unsigned long pump_disabled_until_ms = 0;        // 24 h = 86.4e6 ms, so signed-difference compares are safe
bool forced_run = false;                         // "pump_on": forced run in progress (uses the manual timer, max 5 min)
bool pause_by_command = false;                   // the current pump_in_pause was started by "pump_off" (display wording only)
char screen_msg[48] = "";                        // last remote-command response, shown on the display briefly
uint16_t screen_msg_color = 0xFFFF;              // ST77XX_WHITE
unsigned long screen_msg_until_ms = 0;

// ADC diagnostics for the statistics dump
int16_t last_adc_raw = 0;
float last_adc_vin = 0.0;                        // sensor voltage (0-10 V) behind the divider

// Report scheduling (baseline vs. "water is rising")
SumpReporter reporter;

// Sensor supervision state
uint8_t ads_fail_count = 0;
uint8_t ads_ok_count = 0;
unsigned long last_ads_reinit_ms = 0;

// Network supervision state (see networkTick())
bool ota_started = false;        // ArduinoOTA.begin() has been called (done once WiFi is up)
bool ntp_requested = false;      // configTzTime() has been issued since WiFi came up
unsigned long ntp_request_ms = 0;
unsigned long last_wifi_attempt_ms = 0;
const char* boot_reason = "?";   // Why the chip last reset (shown on boot screen)

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

// (NTP/WiFi retry timing now lives in the "Network supervision" constants near the top.)

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

// Water level (inches). Median-filtered; on a failed read it keeps the last good value.
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
 * Why did the chip last reset? Shown on the boot screen so a watchdog/brownout/crash
 * reset is visible instead of looking like a normal power-up.
 */
const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "CRASH (panic)";
    case ESP_RST_INT_WDT:  return "WATCHDOG (int)";
    case ESP_RST_TASK_WDT: return "WATCHDOG (task)";
    case ESP_RST_WDT:      return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_EXT:      return "reset pin";
    default:               return "other";
  }
}

/**
 * One-word pump state for the display (and, later, for Home Assistant).
 */
const char* pumpStateName() {
  if (ota_in_progress) return "ota";
  if (maintenance_mode) return "maintenance";
  if (pump_on && manual_pump_active) return "manual";
  if (pump_on && pump_extension_active) return "extension";
  if (pump_on) return "auto";
  if (pump_in_pause) return "paused";
  return "idle";
}

/**
 * Write all statistics to flash (NVS skips writes whose value has not changed).
 */
static const char* const DAY_KEYS[5] = {"day0", "day1", "day2", "day3", "day4"};

void saveDailyStats() {
  prefs.putULong("last_start", last_start);
  prefs.putULong("last_dur_sec", last_dur_sec);
  prefs.putULong("last_day_num", last_day_num);
  for (int i = 0; i < 5; i++) {
    prefs.putULong(DAY_KEYS[i], daily_secs[i]);
  }
}

/**
 * Level thresholds: load from flash at boot. Anything missing or invalid falls back to the factory defaults
 * (so a corrupted value can never leave the controller with nonsense thresholds).
 */
void loadLevelParams() {
  SumpLevels L;
  L.trigger = prefs.getFloat("trig", DEFAULT_TRIGGER_INCHES);
  L.off = prefs.getFloat("off", DEFAULT_OFF_INCHES);
  L.alarm = prefs.getFloat("alarm", DEFAULT_ALARM_INCHES);
  if (sumpValidateLevels(L) != NULL) {
    L.trigger = DEFAULT_TRIGGER_INCHES;
    L.off = DEFAULT_OFF_INCHES;
    L.alarm = DEFAULT_ALARM_INCHES;
  }
  trigger_inches = L.trigger;
  off_inches = L.off;
  alarm_inches = L.alarm;
}

/**
 * Apply new (already validated) level thresholds. Each one is written to flash only if it differs from the
 * value in use. Returns true if anything changed.
 */
bool applyLevelParams(const SumpLevels& L) {
  bool changed = false;
  if (fabsf(L.trigger - trigger_inches) > 0.0005f) {
    trigger_inches = L.trigger;
    prefs.putFloat("trig", trigger_inches);
    changed = true;
  }
  if (fabsf(L.off - off_inches) > 0.0005f) {
    off_inches = L.off;
    prefs.putFloat("off", off_inches);
    changed = true;
  }
  if (fabsf(L.alarm - alarm_inches) > 0.0005f) {
    alarm_inches = L.alarm;
    prefs.putFloat("alarm", alarm_inches);
    changed = true;
  }
  return changed;
}

/**
 * Show a short remote-command response on the display for CMD_MESSAGE_MS.
 */
void showScreenMessage(const char* msg, uint16_t color) {
  snprintf(screen_msg, sizeof(screen_msg), "%s", msg);
  screen_msg_color = color;
  screen_msg_until_ms = millis() + CMD_MESSAGE_MS;
  if (screen_msg_until_ms == 0) screen_msg_until_ms = 1;  // 0 means "no message"
}

// Leap days in years 1..y (proleptic Gregorian)
static long leapDaysUpTo(long y) {
  return y / 4 - y / 100 + y / 400;
}

// Convert a YYYYDDD day number (see getDayNumber) to days since 1970-01-01
long dayNumberToDays(uint32_t n) {
  long y = n / 1000;
  long doy = n % 1000;
  return (y - 1970) * 365 + (leapDaysUpTo(y - 1) - leapDaysUpTo(1969)) + (doy - 1);
}

/**
 * Daily statistics rollover. Runs from loop() so "Today" really resets at local midnight
 * (v1.0 only rotated at boot, so "Today" kept growing until the next reboot).
 * If the controller was off for several days, the history shifts by that many days.
 */
void rolloverDailyStats() {
  if (!time_synced) return;
  uint32_t today = getDayNumber();
  if (today == last_day_num) return;

  if (last_day_num != 0) {
    long gap = dayNumberToDays(today) - dayNumberToDays(last_day_num);
    if (gap > 0) {  // gap <= 0 means the clock moved backwards: just re-anchor below
      if (gap > 5) gap = 5;
      for (int i = 4; i >= gap; i--) daily_secs[i] = daily_secs[i - gap];
      for (int i = 0; i < gap; i++) daily_secs[i] = 0;
      if (enaDbgSer) Serial.printf("New day - rotated daily statistics by %ld day(s)\n", gap);
    }
  }
  last_day_num = today;
  saveDailyStats();
}

/**
 * Configure ArduinoOTA callbacks. ArduinoOTA.begin() is deferred to networkTick() until WiFi is up.
 */
void setupOTA() {
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
    disableLoopWDT();  // The update (flash erase/write) blocks loop() for longer than the 5 s watchdog
    if (pump_on) {
      digitalWrite(RELAY_PIN, LOW);  // Turn pump OFF immediately
      pump_on = false;
    }
    manual_pump_active = false;  // Clear manual mode
    forced_run = false;
    mqttFlushCommands();  // commands queued while the update runs must not execute afterwards
    pump_extension_active = false;  // pump_on was just forced off: a half-finished extension must not outlive it
    pump_in_pause = false;
    digitalWrite(BEEPER_PIN, LOW);

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
    unsigned int percent = (total >= 100) ? (progress / (total / 100)) : 0;
    if (enaDbgSer) Serial.printf("Progress: %u%%\r", percent);
    // Update display every 10%
    if (percent % 10 == 0) {
      tft.fillRect(0, 80, 240, 20, ST77XX_BLACK);
      tft.setCursor(0, 80);
      tft.printf("Progress: %u%%", percent);
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    feedLoopWDT();  // An auth failure never ran onStart (watchdog still on); PBKDF2 plus the 3 s delay below could pass 5 s
    ota_in_progress = false;  // Clear OTA flag on error
    mqttFlushCommands();      // anything queued during the failed transfer is stale
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
    enableLoopWDT();  // The sketch keeps running after a failed update: supervise it again
  });
}

/**
 * Non-blocking network supervision, called every pass of loop().
 * Replaces the v1.0 attemptNTPSync(), which could block loop() for up to ~17 s (pump timeouts,
 * the manual timer and level monitoring all stood still) every time it ran.
 *  - Re-issues a WiFi connect every WIFI_RETRY_INTERVAL_MS while disconnected.
 *  - Starts OTA the first time WiFi is up (v1.0 never started OTA if WiFi was down at boot).
 *  - Starts SNTP once per WiFi connection; time_synced follows once the clock is valid.
 */
void networkTick() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    ntp_requested = false;  // ask for NTP again once the link returns
    if (now - last_wifi_attempt_ms >= WIFI_RETRY_INTERVAL_MS) {
      last_wifi_attempt_ms = now;
      if (enaDbgSer) Serial.println("WiFi down - reconnecting");
      WiFi.begin(ssid, password);  // returns immediately; the stack completes the connection
    }
    return;
  }

  if (!ota_started) {
    ArduinoOTA.begin();
    ota_started = true;
    if (enaDbgSer) Serial.println("OTA ready");
  }

  if (!ntp_requested) {
    configTzTime(TZ_INFO, ntpServer);  // asynchronous: returns at once, SNTP runs in the background
    ntp_requested = true;
    ntp_request_ms = now;
    if (enaDbgSer) Serial.println("NTP requested");
  }

  if (!time_synced) {
    time_t t;
    time(&t);
    if (t > 1000000000) {  // Valid timestamp check
      time_synced = true;
      if (enaDbgSer) Serial.println("Time sync SUCCESS");
    } else if (now - ntp_request_ms >= NTP_RESTART_INTERVAL_MS) {
      ntp_requested = false;  // no answer yet - ask again on the next pass
    }
  }
}


void setup() {
  // Safest-first: drive the pump relay OFF before anything else (it floats until configured)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  boot_reason = resetReasonText();

  Serial.begin(115200);
  delay(500);
  if (enaDbgSer) {
    Serial.println("\n\nSump Monitor Controller Starting...");
    Serial.println("ESP32-S2 TFT Version");
    Serial.printf("Reset reason: %s\n", boot_reason);
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
  debugPrint("Initializing...");
  if (enaDbgSer) Serial.println("Display initialized successfully");

  // Initialize ADS1115 (the controller still runs without it, but raises SENSOR FAULT)
  debugPrint("Checking ADS1115...");
  if (ads.begin(ADS_I2C_ADDR)) {
    ads_present = true;
    ads.setGain(GAIN_ONE);  // ±4.096V range (1 bit = 0.125mV)
    debugPrint("ADC: OK (GAIN_ONE)", ST77XX_GREEN);
  } else {
    ads_present = false;
    debugPrint("ADC: NOT FOUND!", ST77XX_RED);
#if ENABLE_DUMMY_SENSOR
    debugPrint("Using dummy values", ST77XX_YELLOW);
#else
    debugPrint("SENSOR FAULT - will retry", ST77XX_YELLOW);
#endif
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

  // WiFi: start connecting, then wait only a few seconds so the boot screen can show the IP.
  // Anything slower is finished by networkTick() in loop() - setup() must not hold up pump
  // control (v1.0 could sit here ~15 s + 2 s for NTP, with the pump unsupervised, after a
  // power outage when the router is also still booting).
  debugPrint("Connecting WiFi...");

  // Hostname and persistence must be set BEFORE WiFi.mode(): the core applies the hostname to the
  // network interface and picks flash-vs-RAM credential storage inside mode().
  WiFi.persistent(false);           // do not rewrite flash with the credentials on every begin()
  WiFi.setHostname(ota_hostname);   // DHCP name "SumpMonitor" instead of esp32s2-xxxxxx
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);      // the stack re-joins by itself after a drop
  WiFi.setSleep(false);             // modem power-save causes latency spikes (a ping took 850 ms) and hurts OTA

  // Using DHCP (router assigns IP automatically)
  // For static IP, uncomment the variables above and this block:
  // if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
  //   Serial.println("Static IP configuration failed");
  // }

  WiFi.begin(ssid, password);
  last_wifi_attempt_ms = millis();

  const unsigned long WIFI_BOOT_WAIT_MS = 5000;  // Cosmetic wait only - loop() keeps trying
  unsigned long wifi_wait_start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifi_wait_start < WIFI_BOOT_WAIT_MS) {
    delay(250);
    if (enaDbgSer) Serial.print(".");
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
  } else {
    if (enaDbgSer) {
      Serial.println();
      Serial.println("WiFi not up yet - continuing; networkTick() will keep retrying");
    }
    debugPrint("WiFi: not up yet", ST77XX_YELLOW);
    debugPrint("(keeps retrying)");
  }

  // OTA callbacks are registered now; ArduinoOTA.begin() and the NTP request are issued by
  // networkTick() the first time WiFi is up (which may be later than boot).
  setupOTA();

  char reason_buf[40];
  snprintf(reason_buf, sizeof(reason_buf), "Reset: %s", boot_reason);
  debugPrint(reason_buf, (esp_reset_reason() == ESP_RST_POWERON || esp_reset_reason() == ESP_RST_SW) ? ST77XX_WHITE : ST77XX_YELLOW);

  // Load preferences from non-volatile storage
  prefs.begin("pump", false);
  last_start = prefs.getULong("last_start", 0);
  last_dur_sec = prefs.getULong("last_dur_sec", 0);
  last_day_num = prefs.getULong("last_day_num", 0);

  for (int i = 0; i < 5; i++) {
    daily_secs[i] = prefs.getULong(DAY_KEYS[i], 0);
  }
  if (enaDbgSer) Serial.printf("Loaded preferences: last_start=%lu, last_dur=%lu sec\n", last_start, last_dur_sec);
  loadLevelParams();  // trigger / off / alarm levels (flash, else factory defaults)
  if (enaDbgSer) Serial.printf("Levels: trigger %.2f, off %.2f, alarm %.2f in\n", trigger_inches, off_inches, alarm_inches);

  reporter.init(REPORT_BASELINE_MS, REPORT_ELEVATED_MS, REPORT_ELEVATED_MAX_MS, REPORT_CALM_MS,
                REPORT_SAMPLE_MS, REPORT_RISE_INCHES, REPORT_NORMAL_INCHES);

  // Daily-statistics rollover is done by rolloverDailyStats() from loop(), as soon as the clock is
  // valid - so it also works when WiFi/NTP comes up after boot, and again at every midnight.

  // Take initial reading before starting pump control
  debugPrint("Reading sensor...");
  delay(500);
  float level = 0.0;
  char level_buf[32];
  if (readWaterLevelChecked(&level)) {
    current_avg_inches = level;
    snprintf(level_buf, sizeof(level_buf), "Initial: %.2f in", current_avg_inches);
    debugPrint(level_buf, ST77XX_CYAN);
    if (enaDbgSer) Serial.printf("Initial water level: %.2f inches\n", current_avg_inches);
  } else {
    // Level unknown: show it, flag the fault now instead of waiting for the loop to count failures
    current_avg_inches = 0.0;
    ads_fail_count = SENSOR_FAULT_READS;
    sensor_fault = true;
    debugPrint("Initial: SENSOR FAULT", ST77XX_RED);
    if (enaDbgSer) Serial.println("Initial water level: SENSOR FAULT");
  }

  // Startup complete (the boot log must stay at 16 lines or fewer: 8 px each on a 135 px screen)
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

  // MQTT / Home Assistant interface runs in its own task (SumpMqtt.ino) so network trouble can
  // never stall this loop. Started after everything it reads (flash settings, statistics) exists.
  mqttBegin();

  // From here on loop() must keep running: if it ever stalls for more than the 5 s task-watchdog
  // timeout (e.g. an I2C device hangs a bus transaction) the chip resets, which drops the pump
  // relay instead of leaving it energised. (The loop-task watchdog is OFF by default in the core.)
  enableLoopWDT();
  if (enaDbgSer) Serial.println("Setup complete - entering main loop\n");
}

void loop() {
  // Handle OTA updates (must be called frequently). OTA itself is started by networkTick() once WiFi is up.
  if (ota_started) ArduinoOTA.handle();

  // Non-blocking WiFi reconnect / NTP / OTA-start supervision
  networkTick();

  // Daily statistics rollover at local midnight (needs a valid clock)
  rolloverDailyStats();

  // Remote commands queued by the MQTT task are executed here, in the same task as all pump logic
  processCommands();

  // Read sensor every 2 seconds
  static unsigned long last_read = 0;
  if (millis() - last_read >= 2000) {
    last_read = millis();

    updateLevelReading();  // sets current_avg_inches / sensor_fault

    // Debug output every 10 readings (20 seconds)
    static int read_count = 0;
    if (++read_count >= 10) {
      if (enaDbgSer) {
        Serial.println("--- Status Update ---");
        Serial.printf("Water level: %.2f in (current reading)\n", current_avg_inches);
        Serial.printf("Sensor: %s\n", sensor_fault ? "FAULT" : (ads_present ? "ADS1115 (real)" : "ADS1115 missing"));
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
          yield();  // Allows background tasks to run
          feedLoopWDT();  // This wait can last up to 10 s, longer than the 5 s loop watchdog

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
          }
          saveDailyStats();

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

    // Also handle alarm acknowledge if an alarm is active.
    // v1.0 set alarm_active=false here, but the high-water check below re-armed it on the very next
    // pass while the level was still high - so the beeper could never be silenced. Now the button
    // MUTES the beeper only; the alarm itself (red LEDs, display, HA) stays until the cause clears,
    // and the beeper sounds again after ALARM_RESOUND_MS if the condition is still present.
    if (alarm_active && !alarm_silenced) {
      if (enaDbgSer) Serial.println("Button 2 pressed - silencing alarm beeper");
      alarm_silenced = true;
      alarm_silenced_ms = millis();
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
    if (!pump_on) {
      // Manual mode with the pump already off is a stale flag (e.g. maintenance's stopPump() ended a run the
      // button had joined). Left set it would block automatic control for the rest of the manual timer.
      manual_pump_active = false;
      manual_press_count = 0;
    } else if ((long)(millis() - manual_pump_end_ms) >= 0) {  // signed math handles millis() overflow
      if (enaDbgSer) Serial.println("Manual pump timer expired - stopping pump");
      stopPump();
      manual_pump_active = false;
      manual_press_count = 0;
    }
  }

  // Daily clock resync at 6:45 AM. configTzTime() only (re)starts SNTP and returns at once - the old
  // code slept 2 s here with the pump unsupervised. (SNTP also re-syncs by itself periodically.)
  if (time_synced && WiFi.status() == WL_CONNECTED) {
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    uint32_t today = getDayNumber();

    // If it's 6:45 AM and we haven't resynced today
    if (tm->tm_hour == CLOCK_SYNC_HOUR && tm->tm_min == CLOCK_SYNC_MINUTE && last_clock_sync_day != today) {
      if (enaDbgSer) Serial.println("Resyncing clock with NTP server...");
      configTzTime(TZ_INFO, ntpServer);
      last_clock_sync_day = today;
    }
  }

  // (NTP retry while unsynchronised is handled, without blocking, by networkTick() above.)

  // Daily maintenance cycle at 7:00 AM (not logged in statistics)
  if (time_synced && !maintenance_mode) {
    // Check if it's 7:00 AM and we haven't run today
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    uint32_t today = getDayNumber();

    // If it's 7:00 AM hour and we haven't run maintenance today
    if (tm->tm_hour == MAINTENANCE_HOUR && last_maintenance_day != today) {
      last_maintenance_day = today;  // exactly one decision per day, whichever way it goes
      if (pump_on || manual_pump_active || pump_in_pause || ota_in_progress || pump_disabled) {
        // The pump is already running (so it IS being exercised), is cooling down after a timeout,
        // is owned by manual/OTA mode, or has been disabled by command. Starting "maintenance" on top of
        // that would (a) lose the run from the statistics and (b) let maintenance's stopPump() cut a
        // real pump-down short - and a disabled pump must stay off.
        if (enaDbgSer) Serial.println("Skipping 7AM maintenance - pump running / busy / disabled");
      } else if (current_avg_inches >= MAINTENANCE_TRIGGER_INCHES) {
        // Start maintenance cycle
        maintenance_mode = true;
        maintenance_start_ms = millis();
        if (enaDbgSer) Serial.printf("Starting 7AM maintenance cycle - water level: %.2f inches\n", current_avg_inches);
        startPump();
      } else {
        // Water level too low, skip but mark as done for today
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

  // A command-disabled pump re-enables itself when its time is up (signed compare: safe across millis() wrap)
  if (pump_disabled && (long)(millis() - pump_disabled_until_ms) >= 0) {
    pump_disabled = false;
    if (enaDbgSer) Serial.println("Pump disable period over - automatic control resumes");
    showScreenMessage("Pump re-enabled (timer)", ST77XX_GREEN);
    mqttPublishEvent("pump_enabled", "disable period ended");
  }
  // ...or the moment the high-water alarm is active (see DISABLE_CANCELS_AT_ALARM)
  if (DISABLE_CANCELS_AT_ALARM && pump_disabled && high_water_alarm) {
    pump_disabled = false;
    if (enaDbgSer) Serial.println("Pump disable CANCELLED - high water alarm");
    showScreenMessage("Disable CANCELLED: high water", ST77XX_RED);
    mqttPublishEvent("pump_enabled", "disable cancelled by the high water alarm");
  }

  // Automatic pump control logic with pause/restart and extension
  // Skipped in maintenance mode, manual/forced mode, and while the pump is disabled by command
  if (!maintenance_mode && !manual_pump_active && !pump_disabled) {
    if (pump_in_pause) {
      // Check if pause period is over
      if (millis() - pump_pause_start_ms >= (PAUSE_SEC * 1000UL)) {
        if (enaDbgSer) Serial.println("Pause complete - restarting pump if still needed");
        pump_in_pause = false;
        pause_by_command = false;
        // Will restart pump on next loop if water still high
      }
    } else if (pump_extension_active) {
      // Handle the short extension after reaching the OFF level (EXTENSION_SEC)
      if (millis() - pump_extension_start_ms >= (EXTENSION_SEC * 1000UL)) {
        if (enaDbgSer) Serial.println("Extension complete - stopping pump");
        stopPump();  // also clears pump_extension_active
        // A normal run that reached the OFF level: the pump is doing its job
        pump_consecutive_timeouts = 0;
        pump_trouble = false;
      }
    } else if (!pump_on && current_avg_inches >= trigger_inches && !(sensor_fault && SENSOR_FAULT_BLOCKS_AUTO_START)) {
      // Start pump when water reaches trigger level. Never start from a frozen level while the sensor is
      // faulted: the pump could run dry on and off indefinitely (see SENSOR_FAULT_BLOCKS_AUTO_START).
      if (enaDbgSer) Serial.printf("Water level %.2f >= %.2f - starting pump\n", current_avg_inches, trigger_inches);
      startPump();

    } else if (pump_on && current_avg_inches <= off_inches && !pump_extension_active) {
      // When reaching the OFF level, start the extension (EXTENSION_SEC)
      if (enaDbgSer) Serial.printf("Water level %.2f <= %.2f - starting %d-second extension\n", current_avg_inches, off_inches, EXTENSION_SEC);
      pump_extension_active = true;
      pump_extension_start_ms = millis();

    } else if (pump_on && (millis() - pump_start_ms) >= (TIMEOUT_SEC * 1000UL)) {
      // Pump has run for 4 minutes
      if (current_avg_inches > off_inches) {
        if (enaDbgSer) Serial.printf("Pump timeout after %d sec, water still at %.2f in - pausing for %d sec\n",
                      TIMEOUT_SEC, current_avg_inches, PAUSE_SEC);
        stopPump();
        pump_in_pause = true;
        pause_by_command = false;
        pump_pause_start_ms = millis();
        // Back-to-back timeouts with the water still high = blocked discharge or inflow beyond
        // the pump's capacity: latch PUMP TROUBLE so somebody is told (alarm + display).
        if (pump_consecutive_timeouts < 255) pump_consecutive_timeouts++;
        if (pump_consecutive_timeouts >= PUMP_TROUBLE_TIMEOUTS) pump_trouble = true;
      }
    }
  }  // End of normal pump control (if !maintenance_mode)

  // Alarm causes. Each is tracked on its own; alarm_active is simply "any cause present".
  // High water uses hysteresis. Skipped while the sensor is faulted (the level is stale then, so
  // the high-water state is left exactly as it was).
  if (!sensor_fault) {
    if (current_avg_inches > alarm_inches) {
      if (!high_water_alarm) {
        if (enaDbgSer) Serial.printf("HIGH WATER ALARM! Level: %.2f inches\n", current_avg_inches);
        high_water_alarm = true;
      }
    } else if (high_water_alarm && current_avg_inches <= alarm_inches - ALARM_HYSTERESIS_INCHES) {
      if (enaDbgSer) Serial.println("Water level dropped - clearing high water alarm");
      high_water_alarm = false;
    }

    // Sump drained to the OFF level: whatever was wrong with the pump, it is draining now
    if (current_avg_inches <= off_inches) {
      pump_consecutive_timeouts = 0;
      pump_trouble = false;
    }
  }

  uint8_t alarm_causes = (high_water_alarm ? 1 : 0) | (pump_trouble ? 2 : 0) | (sensor_fault ? 4 : 0);
  static uint8_t prev_alarm_causes = 0;
  if (alarm_causes & ~prev_alarm_causes) alarm_silenced = false;  // a NEW cause appeared: sound again
  if (alarm_causes == 0) alarm_silenced = false;                  // all clear: re-arm the beeper
  if (alarm_causes != prev_alarm_causes) {
    mqttPublishCauseChange(alarm_causes, prev_alarm_causes);      // push raised/cleared events at once
  }
  prev_alarm_causes = alarm_causes;
  alarm_active = (alarm_causes != 0);

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

  // Push state to MQTT: on any change (pump, alarm, disable...), every 5 min normally, every 2 min while
  // the water is rising or a problem exists (see SumpReporter). Cheap when nothing is due.
  reportingTick();

  // Hourly save of statistics to flash (to prevent loss on power failure)
  if (millis() - last_hourly_save >= 3600000UL) {  // 3600000ms = 1 hour
    last_hourly_save = millis();
    if (enaDbgSer) Serial.println("Performing hourly save of statistics to flash");
    saveDailyStats();
  }

  // Update display every second
  if (millis() - last_display_update >= DISPLAY_INTERVAL) {
    last_display_update = millis();
    updateDisplay();
  }

  delay(100);  // Main loop throttle
}

#if ENABLE_DUMMY_SENSOR
/**
 * BENCH TEST ONLY (ENABLE_DUMMY_SENSOR 1): simulated water level with slow random variation,
 * used when no ADS1115 is wired. Never enable this on the installed controller.
 */
float dummyWaterLevel() {
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

  return result + SENSOR_OFFSET_INCHES;
}
#endif

/**
 * Read the water level (inches) from the ADS1115, with failure detection.
 * Returns true and sets *inches on success; false if the sensor did not answer.
 *
 * Why not ads.readADC_SingleEnded()? That library call ends in an unbounded
 *   while (!conversionComplete());
 * so if the ADS1115 stops answering on I2C (loose wire, EMI from the pump relay) loop() would hang
 * forever with the pump relay still energised. Here the bus is probed first, every conversion has
 * a timeout, and any failure marks the ADC "not present" so updateLevelReading() can count it and
 * try to bring it back.
 *
 * LEVEL_SAMPLES conversions are taken back to back and the MEDIAN is used, which throws away the
 * odd spike caused by relay/pump switching noise.
 */
bool readWaterLevelChecked(float* inches) {
  if (!ads_present) {
    // Missing at boot or lost later: retry begin() periodically (also re-initialises the I2C bus,
    // which recovers a bus left stuck by a glitch).
    if (millis() - last_ads_reinit_ms >= ADS_REINIT_INTERVAL_MS) {
      last_ads_reinit_ms = millis();
      Wire.end();
      Wire.begin(I2C_SDA, I2C_SCL);
      if (ads.begin(ADS_I2C_ADDR)) {
        ads.setGain(GAIN_ONE);
        ads_present = true;
        if (enaDbgSer) Serial.println("ADS1115 (re)initialised");
      }
    }
    if (!ads_present) {
#if ENABLE_DUMMY_SENSOR
      *inches = dummyWaterLevel();
      return true;
#else
      return false;
#endif
    }
  }

  // Is the chip answering at all?
  Wire.beginTransmission(ADS_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    ads_present = false;
    last_ads_reinit_ms = millis() - ADS_REINIT_INTERVAL_MS;  // retry on the very next read
    if (enaDbgSer) Serial.println("ADS1115 did not ACK");
    return false;
  }

  int16_t samples[LEVEL_SAMPLES];
  for (int i = 0; i < LEVEL_SAMPLES; i++) {
    ads.startADCReading(ADS1X15_REG_CONFIG_MUX_SINGLE_0, /*continuous=*/false);
    unsigned long t0 = millis();
    while (!ads.conversionComplete()) {
      if (millis() - t0 > ADS_CONVERSION_TIMEOUT_MS) {
        ads_present = false;
        last_ads_reinit_ms = millis() - ADS_REINIT_INTERVAL_MS;
        if (enaDbgSer) Serial.println("ADS1115 conversion timeout");
        return false;
      }
      delay(1);
    }
    samples[i] = ads.getLastConversionResults();
  }

  // Median (insertion sort - LEVEL_SAMPLES is tiny)
  for (int i = 1; i < LEVEL_SAMPLES; i++) {
    int16_t v = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > v) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = v;
  }
  int16_t adc = samples[LEVEL_SAMPLES / 2];

  float vout = ads.computeVolts(adc);  // Voltage after divider (0-3.33V)

  // Undo voltage divider: 10kΩ/(20kΩ+10kΩ) = 0.333
  // So: Vin = Vout / 0.333 = Vout * 3.0
  float vin = vout * 3.0;
  last_adc_raw = adc;   // kept for the statistics dump
  last_adc_vin = vin;

  // Convert voltage to inches: 10V = 1m = 39.37 inches
  float level = (vin / 10.0) * 39.37;

  // Add sensor offset (sensor mounted above actual bottom), then clamp to the sump's range
  level += SENSOR_OFFSET_INCHES;
  if (level > 18.0) level = 18.0;
  if (level < 0.0) level = 0.0;

  *inches = level;
  return true;
}

/**
 * Take one level reading and update the sensor-fault state. Called every 2 s from loop().
 * On a failed read the last good level is kept (so a one-off I2C glitch cannot flip the pump);
 * after SENSOR_FAULT_READS consecutive failures (~10 s) SENSOR FAULT is raised (alarm + display),
 * and it clears after SENSOR_OK_READS good reads in a row.
 */
void updateLevelReading() {
  float inches;
  if (readWaterLevelChecked(&inches)) {
    current_avg_inches = inches;
    ads_fail_count = 0;
    if (sensor_fault) {
      if (++ads_ok_count >= SENSOR_OK_READS) {
        sensor_fault = false;
        ads_ok_count = 0;
        if (enaDbgSer) Serial.println("Level sensor recovered");
      }
    } else {
      ads_ok_count = 0;
    }
  } else {
    ads_ok_count = 0;
    if (ads_fail_count < 255) ads_fail_count++;
    if (ads_fail_count >= SENSOR_FAULT_READS && !sensor_fault) {
      sensor_fault = true;
      if (enaDbgSer) Serial.println("LEVEL SENSOR FAULT - holding last good level");
    }
  }
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
  // Outside the if: a stale "extension running" flag must clear even when the pump was already off
  // (an OTA start forces the pump off without going through here). Same for a forced run.
  pump_extension_active = false;
  forced_run = false;
}

/**
 * Handle alarm state - flash LED and beep
 */
void handleAlarm() {
  // A muted alarm sounds again if the condition is still there after ALARM_RESOUND_MS
  if (alarm_silenced && (millis() - alarm_silenced_ms >= ALARM_RESOUND_MS)) {
    alarm_silenced = false;
  }

  // Beeper: 2 second cycle, 25% duty (500ms on, 1500ms off) - silent while muted with Button 2
  unsigned long cycle_pos = millis() % 2000;
  digitalWrite(BEEPER_PIN, (!alarm_silenced && cycle_pos < 500) ? HIGH : LOW);

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
  if (sensor_fault) {
    tft.print("Level: ERR");  // the stored level is stale while the sensor is faulted - don't show it as real
  } else {
    tft.printf("Level: %.1f in", current_avg_inches);
  }

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

  // MQTT link indicator (right-justified, under the version): green when connected to the broker.
  // y = 20 leaves three blank pixel rows below the version text (its glyphs end at row 16); the "Last:" line below
  // starts at y = 25 and is never wide enough to reach this column, so there is no overlap.
  tft.setTextColor(mqttIsConnected() ? ST77XX_GREEN : ST77XX_RED);
  const char* mq_text = mqttIsConnected() ? "MQTT ok" : "MQTT --";
  tft.setCursor(240 - (int)strlen(mq_text) * 6, 20);
  tft.print(mq_text);

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

  // Current status (color-coded). Alarm causes take the status line; if the pump is also running
  // it gets its own line below so a high-water event still shows what the pump is doing.
  char status[32];
  uint16_t status_color;
  unsigned long runtime_sec = pump_on ? (millis() - pump_start_ms) / 1000 : 0;

  if (alarm_active) {
    const char* cause = sensor_fault ? "SENSOR FAULT" : (high_water_alarm ? "HIGH WATER!" : "PUMP TROUBLE");
    snprintf(status, sizeof(status), "%s%s", cause, alarm_silenced ? " (mute)" : "");
    status_color = ST77XX_RED;
  } else if (pump_on) {
    snprintf(status, sizeof(status), "PUMP ON (%lus)", runtime_sec);
    status_color = ST77XX_YELLOW;
  } else if (pump_disabled) {
    // Shown instead of WATCHING; the measured level above keeps updating as usual
    snprintf(status, sizeof(status), "PUMP DISABLED");
    status_color = ST77XX_MAGENTA;
  } else if (pump_in_pause) {
    long pause_left = (long)(PAUSE_SEC) - (long)((millis() - pump_pause_start_ms) / 1000);
    if (pause_left < 0) pause_left = 0;
    snprintf(status, sizeof(status), "%s (%lds)", pause_by_command ? "STOPPED" : "COOLING", pause_left);
    status_color = ST77XX_ORANGE;
  } else {
    snprintf(status, sizeof(status), "WATCHING");
    status_color = ST77XX_GREEN;
  }

  tft.setTextSize(2);
  tft.setTextColor(status_color);
  tft.setCursor(0, y);
  tft.print(status);
  y += 25;

  // Pump line: manual/forced countdown (red) - or plain run time / disabled notice when an alarm owns the status line
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

      if (forced_run) {
        tft.printf("FORCED ON (%lus)", remaining_sec);   // remote "pump_on" command
      } else if (in_window) {
        // Show "  ." indicator during window, hide after window expires
        tft.printf("PUMP ON (%lu sec) .", remaining_sec);
      } else {
        tft.printf("PUMP ON (%lu sec)", remaining_sec);
      }
      y += 25;
    }
  } else if (alarm_active && pump_on) {
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(0, y);
    tft.printf("PUMP ON (%lus)", runtime_sec);
    y += 25;
  } else if (alarm_active && pump_disabled) {
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_MAGENTA);
    tft.setCursor(0, y);
    tft.print("PUMP DISABLED");
    y += 25;
  }

  // Info line (size 1, just above the K1/K2 line): a fresh remote-command response, otherwise the
  // time left on a pump disable.
  tft.setTextSize(1);
  if (screen_msg_until_ms != 0 && (long)(millis() - screen_msg_until_ms) >= 0) {
    screen_msg_until_ms = 0;  // expired: clear it, or the signed compare would show the old text again after ~25 days
  }
  if (screen_msg_until_ms != 0) {
    tft.setTextColor(screen_msg_color);
    tft.setCursor(0, 101);
    tft.print(screen_msg);
  } else if (pump_disabled) {
    long left_s = (long)(pump_disabled_until_ms - millis()) / 1000;
    if (left_s < 0) left_s = 0;
    tft.setTextColor(ST77XX_MAGENTA);
    tft.setCursor(0, 101);
    tft.printf("Disabled - back on in %ldh%02ldm", left_s / 3600, (left_s % 3600) / 60);
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
  if (ads_present && !sensor_fault) {
    tft.setTextColor(ST77XX_GREEN);
    tft.print("ADC:OK");
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.print("ADC:FAULT");
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
