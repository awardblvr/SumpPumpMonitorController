/*
 * Sump Monitor Controller - ESP32-S2 Version
 *
 * Adafruit Feather ESP32-S2 TFT based sump pump controller
 * with water level monitoring, automatic pump control, and alarm system
 *
 * Features:
 * - ADS1115 ADC for water level sensing with gain configuration
 * - Automatic pump control with timeout alarm
 * - WiFi connection with timeout to prevent blocking
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
#include <Adafruit_GFX.h>       // Core graphics library
#include <Adafruit_ST7789.h>    // Hardware-specific library for ST7789
#include <SPI.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <Wire.h>

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);  // Display
Adafruit_ADS1115 ads;       // ADC
Adafruit_NeoPixel pixels(1, 33, NEO_GRB + NEO_KHZ800);  // Built-in NeoPixel on GPIO33
Adafruit_NeoPixel externalPixel(1, 11, NEO_GRB + NEO_KHZ800);  // External NeoPixel on GPIO11

Preferences prefs;          // Non-volatile storage

// Hardware status flags
bool ads_present = false;   // Track if ADS1115 is connected
bool external_neo_present = true;  // Set to false if external NeoPixel not installed

// WiFi and Time (PST: UTC-8)
const char* ssid = "Yeah_Right";
const char* password = "u'llNeverGuess";
const long gmtOffset_sec = -8 * 3600;  // PST
const int daylightOffset_sec = 0;      // No DST adjustment
const char* ntpServer = "pool.ntp.org";
bool time_synced = false;  // Track if NTP sync succeeded

// Pin Definitions for Adafruit Feather ESP32-S2 TFT
#define RELAY_PIN 5      // Pump relay (active low) - Connect to external GPIO5
#define BEEPER_PIN 6     // Beeper (active high) - Connect to external GPIO6
#define BTN1_PIN 9       // Manual toggle button - Connect to GPIO9 (pullup enabled)
#define BTN2_PIN 10      // Alarm reset button - Connect to GPIO10 (pullup enabled)
#define NEO_PIN 33       // Built-in NeoPixel (onboard, no external connection needed)
#define I2C_SDA 42       // I2C SDA - Connect ADS1115 SDA to GPIO42 header pin
#define I2C_SCL 41       // I2C SCL - Connect ADS1115 SCL to GPIO41 header pin
// Note: TFT_I2C_POWER (GPIO 21), TFT_BACKLITE (GPIO 45), and NEOPIXEL_POWER (GPIO 34) are defined in board files

// Pump Logic
#define TRIGGER_INCHES 5.0
#define OFF_INCHES 3.0
#define TIMEOUT_SEC 120
bool pump_on = false;
unsigned long pump_start_ms = 0;
bool alarm_active = false;

// Averaging (last 20 readings, 1s intervals)
float readings[20];
int read_idx = 0;
bool buffer_full = false;
float current_avg_inches = 0.0;

// Storage
uint32_t last_start = 0;
uint32_t last_dur_sec = 0;
uint32_t daily_mins[5] = {0};  // Index 0: today, 1: yesterday, ..., 4: 5 days ago
uint32_t last_day_num = 0;

// Display
unsigned long last_display_update = 0;
const unsigned long DISPLAY_INTERVAL = 1000;  // 1s

/**
 * Debug helper: Print message to both Serial and TFT display
 */
void debugPrint(const char* msg, uint16_t color = ST77XX_WHITE) {
  Serial.println(msg);
  tft.setTextColor(color);
  tft.println(msg);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\nSump Monitor Controller Starting...");
  Serial.println("ESP32-S2 TFT Version");

  // CRITICAL: Turn on TFT/I2C power supply FIRST!
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  Serial.println("TFT/I2C power enabled (GPIO 21)");
  delay(10);

  // CRITICAL: Turn on NeoPixel power
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
  Serial.println("NeoPixel power enabled (GPIO 34)");
  delay(10);

  // CRITICAL: Enable TFT backlight
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  Serial.println("TFT backlight enabled (GPIO 45)");
  delay(10);

  // Initialize I2C (must be after TFT_I2C_POWER is enabled)
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.printf("I2C initialized on SDA=%d, SCL=%d\n", I2C_SDA, I2C_SCL);

  // Configure GPIO pins
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);  // Relay off (active high - flipped)
  pinMode(BEEPER_PIN, OUTPUT);
  digitalWrite(BEEPER_PIN, LOW);
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  Serial.println("GPIO pins configured");

  // Initialize display
  Serial.println("Initializing TFT display...");
  tft.init(135, 240);  // ST7789 135x240
  Serial.println("TFT init() completed");

  tft.setRotation(1);  // Correct orientation (landscape)
  Serial.println("TFT rotation set to 1 (landscape)");

  tft.fillScreen(ST77XX_BLACK);
  Serial.println("TFT screen cleared to black");

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);  // Smaller text for debug messages
  tft.setCursor(0, 0);
  Serial.println("TFT text parameters set");

  debugPrint("Sump Monitor v2.0", ST77XX_CYAN);
  debugPrint("ESP32-S2 TFT");
  debugPrint("");
  debugPrint("Initializing...");
  Serial.println("Display initialized successfully");

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

  // Initialize built-in NeoPixel (DISABLED per user request)
  pixels.begin();
  pixels.setBrightness(0);  // Turned off - using external NeoPixel only
  pixels.setPixelColor(0, 0);  // Black (off)
  pixels.show();
  debugPrint("NeoPixel: OFF", ST77XX_GREEN);

  // Initialize external NeoPixel (optional)
  if (external_neo_present) {
    externalPixel.begin();
    externalPixel.setBrightness(50);  // Match built-in brightness
    externalPixel.setPixelColor(0, externalPixel.Color(255, 255, 0));  // Yellow during init
    externalPixel.show();
    debugPrint("Ext NeoPixel: OK", ST77XX_GREEN);
  }

  // WiFi Connection with timeout
  debugPrint("");
  debugPrint("Connecting WiFi...");
  WiFi.begin(ssid, password);

  // CORRECTION: Add timeout to prevent indefinite blocking
  int wifi_attempts = 0;
  const int MAX_WIFI_ATTEMPTS = 30;  // 15 seconds max

  while (WiFi.status() != WL_CONNECTED && wifi_attempts < MAX_WIFI_ATTEMPTS) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected! IP: ");
    Serial.println(WiFi.localIP());
    debugPrint("WiFi: CONNECTED", ST77XX_GREEN);

    char ip_buf[32];
    snprintf(ip_buf, sizeof(ip_buf), "IP: %s", WiFi.localIP().toString().c_str());
    debugPrint(ip_buf);

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
  } else {
    Serial.println();
    Serial.println("WiFi connection failed - continuing without time sync");
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
    daily_mins[i] = prefs.getULong(("day" + String(i)).c_str(), 0);
  }
  Serial.printf("Loaded preferences: last_start=%lu, last_dur=%lu sec\n", last_start, last_dur_sec);

  // Check for new day and rotate daily stats
  if (time_synced) {
    uint32_t today = getDayNumber();
    if (today != last_day_num && last_day_num != 0) {
      Serial.println("New day detected - rotating daily statistics");
      // Shift array (move yesterday's data to index 1, etc.)
      for (int i = 4; i > 0; i--) {
        daily_mins[i] = daily_mins[i - 1];
        prefs.putULong(("day" + String(i)).c_str(), daily_mins[i]);
      }
      daily_mins[0] = 0;  // Reset today's counter
      prefs.putULong("day0", 0);
    }
    last_day_num = today;
    prefs.putULong("last_day_num", today);
  }

  // Initialize readings buffer
  for (int i = 0; i < 20; i++) {
    readings[i] = 0.0;
  }

  // IMPROVEMENT: Take initial readings before starting pump control
  debugPrint("");
  debugPrint("Reading sensor...");
  delay(500);
  for (int i = 0; i < 5; i++) {
    float level = readWaterLevel();
    readings[i] = level;
    read_idx = i + 1;
    delay(200);
  }
  current_avg_inches = calculateAverage();

  char level_buf[32];
  snprintf(level_buf, sizeof(level_buf), "Initial: %.2f in", current_avg_inches);
  debugPrint(level_buf, ST77XX_CYAN);
  Serial.printf("Initial water level: %.2f inches\n", current_avg_inches);

  // Startup complete
  debugPrint("");
  debugPrint("READY!", ST77XX_GREEN);
  pixels.setPixelColor(0, 0);  // Built-in off
  pixels.show();
  if (external_neo_present) {
    externalPixel.setPixelColor(0, externalPixel.Color(0, 255, 0));  // Green = ready
    externalPixel.show();
  }

  delay(3000);  // Show init screen longer
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("Setup complete - entering main loop\n");
}

void loop() {
  // Read sensor every 1 second
  static unsigned long last_read = 0;
  if (millis() - last_read >= 1000) {
    last_read = millis();

    float inches = readWaterLevel();
    readings[read_idx % 20] = inches;
    read_idx++;
    if (read_idx >= 20) buffer_full = true;

    current_avg_inches = calculateAverage();

    // Debug output every 10 seconds
    static int read_count = 0;
    if (++read_count >= 10) {
      Serial.println("--- Status Update ---");
      Serial.printf("Water level: %.2f in (avg of %d readings)\n",
                    current_avg_inches, buffer_full ? 20 : read_idx);
      Serial.printf("Sensor: %s\n", ads_present ? "ADS1115 (real)" : "DUMMY mode");
      Serial.printf("Pump: %s", pump_on ? "ON" : "OFF");
      if (pump_on) {
        Serial.printf(" (runtime: %lu sec)", (millis() - pump_start_ms) / 1000);
      }
      Serial.println();
      Serial.printf("Alarm: %s\n", alarm_active ? "ACTIVE" : "inactive");
      Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
      Serial.println();
      read_count = 0;
    }
  }

  // Button handling with simple debouncing
  static unsigned long last_btn1_press = 0;
  static unsigned long last_btn2_press = 0;

  // Button 1: Manual pump toggle
  if (digitalRead(BTN1_PIN) == LOW && (millis() - last_btn1_press > 500)) {
    last_btn1_press = millis();
    Serial.println("Button 1 pressed - toggling pump manually");
    togglePumpManual();
    delay(50);  // Debounce
    while (digitalRead(BTN1_PIN) == LOW) {
      delay(10);  // Wait for release
    }
  }

  // Button 2: Alarm reset
  if (digitalRead(BTN2_PIN) == LOW && alarm_active && (millis() - last_btn2_press > 500)) {
    last_btn2_press = millis();
    Serial.println("Button 2 pressed - resetting alarm");
    alarm_active = false;
    pixels.setPixelColor(0, 0);  // Built-in off
    pixels.show();
    if (external_neo_present) {
      externalPixel.setPixelColor(0, externalPixel.Color(0, 255, 0));  // Green
      externalPixel.show();
    }
    digitalWrite(BEEPER_PIN, LOW);
    delay(50);
    while (digitalRead(BTN2_PIN) == LOW) {
      delay(10);
    }
  }

  // Automatic pump control logic
  if (!pump_on && current_avg_inches >= TRIGGER_INCHES) {
    // Start pump when water reaches trigger level
    Serial.printf("Water level %.2f >= %.2f - starting pump\n", current_avg_inches, TRIGGER_INCHES);
    startPump();

  } else if (pump_on && current_avg_inches <= OFF_INCHES) {
    // Stop pump when water drops to off level
    Serial.printf("Water level %.2f <= %.2f - stopping pump\n", current_avg_inches, OFF_INCHES);
    stopPump();

  } else if (pump_on && (millis() - pump_start_ms) >= (TIMEOUT_SEC * 1000UL)) {
    // CORRECTION: Check for timeout alarm condition
    if (current_avg_inches > OFF_INCHES) {
      Serial.printf("ALARM! Pump timeout after %d sec, water still at %.2f in\n",
                    TIMEOUT_SEC, current_avg_inches);
      alarm_active = true;
      stopPump();  // CORRECTION: Stop pump when alarm triggers
    }
  }

  // Alarm handling
  if (alarm_active) {
    handleAlarm();
  } else {
    digitalWrite(BEEPER_PIN, LOW);

    // Normal NeoPixel indication (built-in off, external shows status)
    // Green: low level (< 6"), Blue: medium-high level (6-8"), Purple: very high (>8")
    uint32_t color;
    if (current_avg_inches < 6.0) {
      color = externalPixel.Color(0, 255, 0);  // Green - normal
    } else if (current_avg_inches < 8.0) {
      color = externalPixel.Color(0, 0, 255);  // Blue - getting high
    } else {
      color = externalPixel.Color(128, 0, 128);  // Purple - very high (pump should be running)
    }

    // Soft blink (500ms on, 500ms off) - external NeoPixel only
    if (millis() % 1000 < 500) {
      pixels.setPixelColor(0, 0);  // Built-in always off
      if (external_neo_present) {
        externalPixel.setPixelColor(0, color);
      }
    } else {
      pixels.setPixelColor(0, 0);  // Built-in always off
      if (external_neo_present) {
        externalPixel.setPixelColor(0, 0);
      }
    }
    pixels.show();
    if (external_neo_present) {
      externalPixel.show();
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
      Serial.printf("[DUMMY] New target level: %.2f inches\n", target_level);
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

    return result;
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

  return inches;
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

    if (time_synced) {
      time_t now;
      time(&now);
      last_start = now;
      Serial.printf("Pump started at %lu\n", last_start);
    } else {
      Serial.println("Pump started (no timestamp - WiFi not connected)");
    }
  }
}

/**
 * Stop the pump and save statistics
 */
void stopPump() {
  if (pump_on) {
    pump_on = false;
    digitalWrite(RELAY_PIN, LOW);  // Turn relay OFF (flipped logic)

    unsigned long dur_ms = millis() - pump_start_ms;
    last_dur_sec = dur_ms / 1000;

    // Save to preferences
    prefs.putULong("last_dur_sec", last_dur_sec);

    // Add to today's total (note: loses partial minutes)
    daily_mins[0] += last_dur_sec / 60;
    prefs.putULong("day0", daily_mins[0]);

    // Store start time
    prefs.putULong("last_start", last_start);

    Serial.printf("Pump stopped after %lu seconds (%lu min)\n",
                  last_dur_sec, last_dur_sec / 60);
  }
}

/**
 * Toggle pump manually via button
 */
void togglePumpManual() {
  if (pump_on) {
    Serial.println("Manual pump OFF");
    stopPump();
  } else {
    Serial.println("Manual pump ON");
    startPump();
  }
}

/**
 * Handle alarm state - flash LED and beep
 */
void handleAlarm() {
  // Beeper: 2 second cycle, 25% duty (500ms on, 1500ms off)
  unsigned long cycle_pos = millis() % 2000;
  digitalWrite(BEEPER_PIN, (cycle_pos < 500) ? HIGH : LOW);

  // NeoPixel: Flash red at 1Hz (external only, built-in off)
  uint32_t red_color = (millis() % 1000 < 500) ? externalPixel.Color(255, 0, 0) : externalPixel.Color(0, 0, 0);
  pixels.setPixelColor(0, 0);  // Built-in always off
  pixels.show();

  if (external_neo_present) {
    externalPixel.setPixelColor(0, red_color);
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
  y += 25;

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  // Last pump run information
  if (time_synced && last_start > 0 && last_dur_sec > 0) {
    struct tm *tm = localtime((time_t*)&last_start);
    char buf[64];
    strftime(buf, sizeof(buf), "%m/%d/%y %I:%M%p", tm);  // Fixed: 11/04/25 11:48PM format

    tft.setCursor(0, y);
    tft.printf("Last: %s", buf);
    y += 15;

    tft.setCursor(0, y);
    tft.printf("Duration: %lu min", last_dur_sec / 60);
    y += 15;
  } else if (!time_synced) {
    tft.setCursor(0, y);
    tft.println("Last run: (no time)");
    y += 15;
  }

  // 5-day total runtime
  uint32_t total_mins = 0;
  for (int i = 0; i < 5; i++) {
    total_mins += daily_mins[i];
  }

  tft.setCursor(0, y);
  tft.printf("5-day total: %lu min", total_mins);
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
    status = "IDLE";
    status_color = ST77XX_GREEN;
  }

  tft.setTextSize(2);
  tft.setTextColor(status_color);
  tft.setCursor(0, y);
  tft.print(status);

  // Debug info at bottom
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(0, 110);
  tft.printf("Readings: %d", buffer_full ? 20 : read_idx);

  // Show sensor status
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
    tft.printf("WiFi:%ddBm", rssi);
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.print("WiFi:--");
  }
}
