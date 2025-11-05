# Sump Monitor Controller - Complete Setup Guide
## Adafruit Feather ESP32-S2 TFT Version

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Software Installation](#software-installation)
3. [Library Installation](#library-installation)
4. [Hardware Assembly](#hardware-assembly)
5. [Code Configuration](#code-configuration)
6. [Uploading and Testing](#uploading-and-testing)
7. [Calibration and Tuning](#calibration-and-tuning)
8. [Installation and Deployment](#installation-and-deployment)

---

## Prerequisites

---

## Software Installation

### Step 1: Install Arduino IDE

1. **Download Arduino IDE 2.x** from https://www.arduino.cc/en/software
   - Recommended: Version 2.0 or later
   - Alternative: Arduino IDE 1.8.19 (legacy) also works

2. **Install the IDE** following the installer prompts for your operating system

3. **Launch Arduino IDE** and verify it opens successfully

### Step 2: Install ESP32 Board Support

1. **Open Preferences/Settings:**
   - macOS: `Arduino IDE` → `Settings...`
   - Windows/Linux: `File` → `Preferences`

2. **Add ESP32 Board Manager URL:**
   - Find the field: "Additional boards manager URLs"
   - Add this URL (if there are existing URLs, separate with comma):
     ```
     https://espressif.github.io/arduino-esp32/package_esp32_index.json
     ```
   - Click **OK**

3. **Open Boards Manager:**
   - Click the Boards Manager icon (on left sidebar)
   - OR: `Tools` → `Board` → `Boards Manager...`

4. **Install ESP32 package:**
   - Search for: `esp32`
   - Find: **"esp32 by Espressif Systems"**
   - Select version **2.0.11 or later** (or 3.x if available)
   - Click **Install**
   - Wait for download/installation to complete (may take several minutes)

5. **Select your board:**
   - Go to: `Tools` → `Board` → `esp32`
   - Select: **"Adafruit Feather ESP32-S2 TFT"**
   - If not available, select: **"ESP32S2 Dev Module"**

   **Important board settings (if using ESP32S2 Dev Module):**
   - `Tools` → `USB CDC On Boot` → **Enabled**
   - `Tools` → `USB DFU On Boot` → **Disabled**
   - `Tools` → `Flash Mode` → **QIO**
   - `Tools` → `Flash Size` → **4MB (32Mb)**
   - `Tools` → `Partition Scheme` → **Default 4MB with spiffs**
   - `Tools` → `PSRAM` → **Enabled** (if using ESP32S2 Dev Module)
   - `Tools` → `Upload Speed` → **921600**

6. **Select COM port:**
   - Connect your Feather ESP32-S2 TFT via USB-C
   - Go to: `Tools` → `Port`
   - Select the port showing your board (e.g., `COM3`, `/dev/ttyUSB0`, `/dev/cu.usbmodem*`)

---

## Library Installation

You need to install the following libraries through the Arduino Library Manager:

### Step 1: Open Library Manager

- Click the Library Manager icon (books icon on left sidebar)
- OR: `Tools` → `Manage Libraries...`

### Step 2: Install Adafruit GFX Library

1. Search for: `Adafruit GFX`
2. Find: **"Adafruit GFX Library by Adafruit"**
3. Click **Install** (install latest version)

### Step 3: Install Adafruit ST7789 Library

1. Search for: `Adafruit ST7789`
2. Find: **"Adafruit ST7789 Library by Adafruit"**
3. Click **Install** (install latest version)

### Step 4: Install Adafruit ADS1X15

1. Search for: `Adafruit ADS1X15`
2. Find: **"Adafruit ADS1X15 by Adafruit"**
3. Click **Install**
4. When prompted to install dependencies (Adafruit BusIO), click **Install All**

### Step 5: Install Adafruit NeoPixel

1. Search for: `Adafruit NeoPixel`
2. Find: **"Adafruit NeoPixel by Adafruit"**
3. Click **Install** (install latest version 1.11+)
4. Click **Install All** if dependencies are requested

### Step 6: Verify Built-in Libraries

These libraries are included with ESP32 board package (no installation needed):
- ✓ WiFi.h
- ✓ Wire.h (I2C)
- ✓ SPI.h
- ✓ time.h
- ✓ Preferences.h (non-volatile storage)

### Library Installation Summary

| Library | Version | Purpose |
|---------|---------|---------|
| Adafruit GFX | Latest | Core graphics library |
| Adafruit ST7789 | Latest | ST7789 TFT display driver |
| Adafruit ADS1X15 | 2.5+ | ADS1115 ADC interface |
| Adafruit NeoPixel | 1.11+ | WS2812B LED control |
| Preferences | Built-in | Non-volatile data storage |
| WiFi | Built-in | Network connectivity |
| Wire | Built-in | I2C communication |

**Note:** The Adafruit ST7789 library handles all display configuration automatically - no manual setup required!

---

## Hardware Assembly

Follow the detailed instructions in [`feather_wiring_diagram.md`](feather_wiring_diagram.md).

### Assembly Checklist

- [ ] Voltage divider circuit built (20kΩ, 10kΩ resistors)
- [ ] Noise filter capacitor added (2.2µF)
- [ ] ADS1115 connected via I2C (GPIO 41=SCL, 42=SDA)
- [ ] Relay module connected to GPIO 5
- [ ] Beeper connected to GPIO 6
- [ ] Button 1 connected to GPIO 9
- [ ] Button 2 connected to GPIO 10
- [ ] Built-in NeoPixel verified (no wiring needed)
- [ ] All grounds connected to common point
- [ ] Power distribution verified (3.3V, USB/5V, GND)

### Pre-Power-On Safety Checks

Before connecting power:

1. **Continuity test:** All GNDs are common
2. **Isolation test:** No short between power rails and GND
3. **Polarity check:** Electrolytic capacitor polarity correct
4. **Voltage divider test:** Measure ~30kΩ from sensor input to GND
5. **Visual inspection:** No solder bridges, all connections secure

---

## Code Configuration

### Step 1: Download the Code

Get the code file: `SumpMonitorController_Corrected.ino`

### Step 2: Open in Arduino IDE

1. Double-click the `.ino` file, or
2. `File` → `Open` → select the file

### Step 3: Configure WiFi Credentials

Find these lines (around line 39-40):

```cpp
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
```

**Replace with your actual WiFi credentials:**

```cpp
const char* ssid = "MyHomeWiFi";
const char* password = "MySecurePassword123";
```

⚠️ **Important:** ESP32-S2 only supports **2.4GHz WiFi**, not 5GHz networks.

### Step 4: Verify Pin Definitions

The code should already have these pin definitions for the Feather ESP32-S2 TFT:

```cpp
#define RELAY_PIN 5      // Pump relay (active low)
#define BEEPER_PIN 6     // Beeper (active high)
#define BTN1_PIN 9       // Manual toggle button
#define BTN2_PIN 10      // Alarm reset button
#define NEO_PIN 33       // Built-in NeoPixel
#define I2C_SDA 3        // I2C SDA (STEMMA QT)
#define I2C_SCL 4        // I2C SCL (STEMMA QT)
```

If your code shows different values, update them to match above.

### Step 5: Verify Pump Trigger Levels

Check these settings (around line 58-60):

```cpp
#define TRIGGER_INCHES 8.0   // Start pump at this water level
#define OFF_INCHES 4.0       // Stop pump at this water level
#define TIMEOUT_SEC 120      // Alarm if pump runs this long
```

**Adjust if needed** based on your sump pit dimensions and requirements.

### Step 6: Optional Customizations

#### Change Time Zone

Default is PST (UTC-8). To change (around line 41):

```cpp
const long gmtOffset_sec = -8 * 3600;  // PST
```

Examples:
- EST: `-5 * 3600`
- CST: `-6 * 3600`
- MST: `-7 * 3600`
- PST: `-8 * 3600`
- UTC: `0`

#### Adjust NeoPixel Brightness

Default is 50 (out of 255). Find in `setup()` around line 125:

```cpp
pixels.setBrightness(50);  // 0 = off, 255 = max
```

Increase for brighter LED, decrease to save power / reduce brightness.

#### Change Display Update Rate

Default is 1 second. Find around line 77:

```cpp
const unsigned long DISPLAY_INTERVAL = 1000;  // milliseconds
```

---

## Uploading and Testing

### Step 1: Connect Board

1. Connect Feather ESP32-S2 TFT to computer via USB-C cable
2. Verify `Tools` → `Port` shows correct port
   - macOS: `/dev/cu.usbmodem*` or `/dev/tty.usbmodem*`
   - Windows: `COM3`, `COM4`, etc.
   - Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`

### Step 2: Select Board and Settings

Verify these settings:

- `Tools` → `Board` → **"Adafruit Feather ESP32-S2 TFT"** (or ESP32S2 Dev Module)
- `Tools` → `USB CDC On Boot` → **Enabled**
- `Tools` → `Upload Speed` → **921600** (or slower if upload fails)
- `Tools` → `Port` → Your board's port

### Step 3: Compile Code

1. Click **Verify** button (checkmark icon)
2. Wait for compilation to complete
3. **Expected:** "Done compiling" message with no errors
4. **If errors:** Check library installation and pin definitions

Common errors:
- `'TFT_eSPI' was not declared` → Reinstall TFT_eSPI library
- `'ads' does not name a type` → Install Adafruit ADS1X15 library
- `'pixels' was not declared` → Install Adafruit NeoPixel library

### Step 4: Upload Code

1. Click **Upload** button (arrow icon)
2. **If upload fails with "Connecting..." error:**

   **ESP32-S2 Manual Reset Procedure:**
   - Unplug USB cable
   - Hold **BOOT** button (small button near USB connector)
   - While holding BOOT, plug in USB cable
   - Keep holding BOOT
   - Click **Upload** in Arduino IDE
   - Release **BOOT** button when upload starts

   Alternative:
   - Press and hold **BOOT** button
   - Press and release **RESET** button (if available)
   - Release **BOOT** button
   - Click **Upload**

3. **Expected:** Progress bar, then "Done uploading"

4. **After upload:** The board will automatically restart and run the program

### Step 5: Open Serial Monitor

1. Click **Serial Monitor** button (magnifying glass icon) in Arduino IDE
2. Set baud rate to: **115200**
3. **Expected startup output:**

```
Sump Monitor Controller Starting...
I2C initialized on SDA=3, SCL=4
GPIO pins configured
Display initialized
ADS1115 initialized with GAIN_ONE (±4.096V)
ADC: OK
NeoPixel initialized
WiFi...
WiFi connected!
IP: 192.168.1.XXX
NTP time synced
Time: Synced
Reading sensor...
Initial water level: 0.52 inches
Setup complete - entering main loop
```

### Step 6: Verify Display

**Display should show:**
- "Sump Monitor" title
- "Initializing..." during startup
- Initialization status for each component
- After startup: Current water level and status

**Built-in NeoPixel should:**
- Show yellow during initialization
- Show green when ready (normal operation)
- Be visible on the back side of the board

### Step 7: Test Buttons

**Button 1 (GPIO 9) - Manual Pump Toggle:**
1. Press button
2. **Expected:**
   - Serial: "Button 1 pressed - toggling pump manually"
   - Relay clicks
   - Display shows "PUMP ON (Xs)"
   - NeoPixel color changes
3. Press again to turn off

**Button 2 (GPIO 10) - Alarm Reset:**
1. Only works when alarm is active
2. Will test later during full system test

---

## Calibration and Tuning

### Step 1: Verify Voltage Divider

1. **Disconnect sensor** from voltage divider
2. Connect variable power supply (or battery pack) to voltage divider input
3. Measure voltage at ADS1115 A0 pin with multimeter:

| Input Voltage | Expected A0 Voltage | Expected Display Reading |
|---------------|---------------------|--------------------------|
| 0.0V | 0.0V | ~0.0 inches |
| 3.3V | 1.1V | ~13.0 inches |
| 5.0V | 1.67V | ~19.7 inches |
| 10.0V | 3.33V | 18.0 inches (clamped) |

### Step 2: Sensor Calibration

If readings are incorrect:

#### Problem: Reading too high
- Check voltage divider resistor values (should be 20kΩ and 10kΩ)
- Verify sensor is outputting correct voltage
- Measure sensor output with multimeter

#### Problem: Reading too low
- Check for poor connections in voltage divider
- Verify ADS1115 gain is set to `GAIN_ONE` in code
- Check sensor power supply

#### Fine-Tune Scaling

If sensor has slightly different scaling, adjust line 346 in code:

```cpp
float inches = (vin / 10.0) * 39.37;  // 10V = 1m = 39.37 inches
```

Example: If your sensor outputs 9V at full scale (1m):

```cpp
float inches = (vin / 9.0) * 39.37;  // 9V = 1m
```

### Step 3: Test Averaging Algorithm

1. Place sensor in water (or apply variable voltage)
2. Create disturbance (pump on, add/remove water)
3. Watch serial monitor for readings every 10 seconds
4. **Expected:** Readings stabilize within 20 seconds (20-sample average)

If too jumpy, increase buffer size in code (line 63):

```cpp
float readings[30];  // Increase from 20 to 30 for more averaging
```

Don't forget to update related code that references the buffer size!

### Step 4: Display Orientation Test

If the display orientation is wrong:

In code, find `tft.setRotation(1)` and try:
- `tft.setRotation(0)` - Portrait
- `tft.setRotation(1)` - Landscape (default)
- `tft.setRotation(2)` - Portrait inverted
- `tft.setRotation(3)` - Landscape inverted

### Step 5: Pump Timing Test

1. **Lower trigger level temporarily** to something testable:
   ```cpp
   #define TRIGGER_INCHES 2.0  // For testing
   #define OFF_INCHES 1.0
   ```

2. Upload modified code

3. Place sensor in water, raise level above 2 inches

4. **Expected sequence:**
   - Display shows increasing water level
   - At 2.0": Pump turns on (relay clicks)
   - Status shows "PUMP ON (Xs)"
   - Lower water level below 1.0"
   - Pump turns off

5. **Restore original trigger levels** and re-upload

---

## Installation and Deployment

---

## Troubleshooting

### WiFi Connection Issues

#### "WiFi: FAILED" on Display
- **Cause:** Wrong credentials, 5GHz network, or signal too weak
- **Fix:**
  - Verify SSID and password are correct in code
  - Ensure using 2.4GHz WiFi network (not 5GHz)
  - Move closer to router during setup
  - Check router settings (WPA2 recommended, avoid enterprise WiFi)

#### WiFi Connects Then Disconnects
- **Cause:** Weak signal or router DHCP issue
- **Fix:**
  - Move closer to WiFi access point
  - Use WiFi extender
  - Assign static IP in code

#### Time Shows Wrong Time Zone
- **Cause:** Incorrect `gmtOffset_sec` setting
- **Fix:** Adjust `gmtOffset_sec` in code for your time zone

#### Time Resets to 1970
- **Cause:** NTP sync failed
- **Fix:**
  - Check WiFi connection
  - Verify internet access (try pinging pool.ntp.org from computer)
  - System will auto-retry NTP sync when WiFi reconnects

### Sensor Reading Issues

#### Always Reads 0 or Very Low
- **Cause:** Voltage divider not working, sensor unpowered, or bad connection
- **Fix:**
  - Check voltage divider resistor values with multimeter (20kΩ and 10kΩ)
  - Verify sensor has power (usually 12-24VDC)
  - Test sensor output voltage directly (should be 0-10V)
  - Check ADS1115 I2C connection
  - Verify ADS1115 address is 0x48

#### Always Reads 18 (Maximum)
- **Cause:** Short circuit in voltage divider or ADS1115 issue
- **Fix:**
  - Check for solder bridges in voltage divider circuit
  - Verify R1 (20kΩ) is not shorted
  - Test ADS1115 with known voltage source
  - Try different ADS1115 channel (A1, A2, A3)

#### Erratic/Jumpy Readings
- **Cause:** Noise, poor filtering, or loose connections
- **Fix:**
  - Verify filter capacitor is installed (2.2µF)
  - Check for loose breadboard connections (solder for permanent installation)
  - Increase averaging window in code
  - Add shielding to sensor cable (ground shield at controller end only)
  - Move sensor cable away from pump power wires
  - Check for water in sensor connections

#### Reading Drifts Over Time
- **Cause:** Temperature drift or sensor fouling
- **Fix:**
  - Clean sensor diaphragm
  - Verify sensor is rated for water temperature
  - Check for sensor aging (pressure transducers can drift over years)

### General System Issues

#### Random Reboots
- **Cause:** Power supply insufficient, brownout, or code crash
- **Fix:**
  - Use 5V/2A power supply minimum
  - Check for loose USB cable
  - Add bulk capacitor (470-1000µF) across USB and GND
  - Check serial monitor for crash/exception messages
  - Add more delay() in loop if needed

#### System Freezes
- **Cause:** Infinite loop, I2C bus hang, or WiFi issue
- **Fix:**
  - Add watchdog timer in code
  - Check I2C wiring (SDA/SCL)
  - Disable WiFi temporarily to isolate issue
  - Add debug Serial.println() statements

#### Preferences Not Saving
- **Cause:** NVS partition issue or power loss during write
- **Fix:**
  - Verify partition scheme includes NVS
  - Erase flash completely and re-upload
  - Check for `prefs.begin("pump", false)` in setup()

---

## Advanced Customization

### Add WiFi Manager (No Hardcoded Credentials)

Install `WiFiManager` library:
```cpp
#include <WiFiManager.h>

void setup() {
  WiFiManager wm;
  wm.autoConnect("SumpMonitor");  // Creates AP if can't connect
  ...
}
```

### Add MQTT for Home Automation

Install `PubSubClient` library and publish sensor data:

```cpp
#include <PubSubClient.h>

WiFiClient espClient;
PubSubClient mqtt(espClient);

void loop() {
  char msg[50];
  snprintf(msg, 50, "%.2f", current_avg_inches);
  mqtt.publish("sump/water_level", msg);
  ...
}
```

### Add Web Server for Remote Monitoring

```cpp
#include <WebServer.h>

WebServer server(80);

void handleRoot() {
  String html = "<h1>Sump Monitor</h1>";
  html += "<p>Water Level: " + String(current_avg_inches) + " inches</p>";
  server.send(200, "text/html", html);
}

void setup() {
  ...
  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  server.handleClient();
  ...
}
```

### Add Remote Control via Telegram Bot

Use `UniversalTelegramBot` library to control and monitor via Telegram app.

---

## Support and Resources

### Official Documentation
- **Adafruit Feather ESP32-S2 TFT:** https://learn.adafruit.com/adafruit-esp32-s2-tft-feather
- **ESP32-S2 Datasheet:** https://www.espressif.com/en/products/socs/esp32-s2
- **ESP32 Arduino Core:** https://docs.espressif.com/projects/arduino-esp32/
- **TFT_eSPI Library:** https://github.com/Bodmer/TFT_eSPI
- **Adafruit ADS1X15:** https://github.com/adafruit/Adafruit_ADS1X15

### Community Forums
- **Adafruit Forums:** https://forums.adafruit.com/
- **ESP32 Forum:** https://esp32.com/
- **Arduino Forum:** https://forum.arduino.cc/

### Tutorials
- **Adafruit Learn Guides:** https://learn.adafruit.com/
- **Random Nerd Tutorials (ESP32):** https://randomnerdtutorials.com/getting-started-with-esp32/

---

## Revision History

| Date | Version | Changes |
|------|---------|---------|
| 2025-11-03 | 2.0 | Adapted for Adafruit Feather ESP32-S2 TFT |

---

## Safety Disclaimers

⚠️ **ELECTRICAL SAFETY**
- High voltage (120VAC or 240VAC) can cause serious injury or death
- Only qualified electricians should work with AC mains power
- Always disconnect power before servicing
- Use proper GFCI protection for all pump circuits
- Follow all local electrical codes (NEC, local amendments)
- Never bypass safety features

⚠️ **WATER SAFETY**
- Ensure all electronics are properly enclosed and waterproofed
- Use IP65 or higher rated enclosures for damp locations
- Keep controller above maximum possible water level
- Install emergency manual pump override switch
- Do not submerge controller or relay module

⚠️ **RELIABILITY**
- This system is for monitoring and convenience only
- **DO NOT** rely solely on this system for flood prevention
- Install a backup mechanical float switch for emergency pump activation
- Test system regularly (weekly recommended)
- Maintain your sump pump according to manufacturer recommendations
- Consider redundant pump system for critical applications
- Have emergency plan for controller failure

---

**Document created by:** Claude Code
**For project:** Sump Monitor Controller
**Target board:** Adafruit Feather ESP32-S2 TFT
**Based on:** Original LilyGo T-Display S3 setup guide
**Difficulty level:** Intermediate to Advanced
**Estimated setup time:** 3-6 hours (including testing)
