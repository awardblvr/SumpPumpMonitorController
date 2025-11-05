# Sump Monitor Controller
**ESP32-S2 Version for Adafruit Feather ESP32-S2 TFT**

## Overview
Automatic sump pump controller with water level monitoring, timeout protection, and alarm system.

## Hardware Requirements
- **Adafruit Feather ESP32-S2 TFT** (240x135 ST7789 display, built-in NeoPixel)
- **ADS1115** 16-bit ADC module (I2C)
- **0-10V Water Level Sensor** with voltage divider (see PIN_CONNECTIONS.md)
- **5V Relay Module** (active-low)
- **Piezo Beeper**
- **2 Push Buttons** (NO, momentary)

## Features
- **Automatic Pump Control**: Turns pump on at 8" water level, off at 4"
- **Timeout Protection**: 120-second maximum runtime with alarm
- **Visual Status**: Built-in NeoPixel shows system status (Green=OK, Red=Alarm, Blue=High Water)
- **Audible Alarm**: Beeper sounds if pump times out
- **Historical Data**: Tracks pump runtime for last 5 days
- **WiFi Time Sync**: Timestamps pump runs via NTP
- **Manual Override**: Button for manual pump control
- **TFT Display**: Real-time water level, status, and statistics

## Quick Start
1. **Install Arduino Libraries**:
    - TFT_eSPI
    - Adafruit_ADS1X15
    - Adafruit_NeoPixel
    - WiFi (built-in)
    - Preferences (built-in)

2. **Configure TFT_eSPI**:
    - Copy `User_Setup.h` to your TFT_eSPI library folder
    - Or add to `User_Setup_Select.h`: `#include "User_Setup.h"`

3. **Wire Hardware**: See `PIN_CONNECTIONS.md` for complete wiring guide

4. **Configure Sketch**:
    - Edit WiFi credentials in sketch (lines 39-40)
    - Adjust thresholds if needed (TRIGGER_INCHES, OFF_INCHES, TIMEOUT_SEC)

5. **Upload**:
    - Select Board: **Adafruit Feather ESP32-S2 TFT**
    - Upload `SumpMonitorController_Corrected.ino`

## Pin Assignments
| Function | GPIO | Notes |
|----------|------|-------|
| Relay (Pump) | 5 | Active-LOW |
| Beeper | 6 | Active-HIGH |
| Button 1 (Toggle) | 9 | Pullup, press=LOW |
| Button 2 (Reset) | 10 | Pullup, press=LOW |
| NeoPixel | 33 | Built-in |
| I2C SDA | 3 | STEMMA QT |
| I2C SCL | 4 | STEMMA QT |

## Operation
- **Green LED**: Normal operation, water level < 6"
- **Blue LED**: Water level 6-8" (approaching trigger point)
- **Purple LED**: Water level > 8" (pump should be running)
- **Red Flashing + Beeper**: ALARM - pump timeout
- **Button 1**: Manual pump on/off toggle
- **Button 2**: Reset alarm (clears red LED and beeper)

## Safety Notes
⚠️ **Important**:
- Pump must have separate power supply (DO NOT power from board)
- Use voltage divider for 10V sensor input (see PIN_CONNECTIONS.md)
- Relay module needs 5V supply (USB or external)
- Test thoroughly before deploying in critical applications

## Files
- `SumpMonitorController_Corrected.ino` - Main sketch (USE THIS)
- `SumpMonitorController.ino` - Deprecated (LilyGo version)
- `User_Setup.h` - TFT_eSPI configuration
- `PIN_CONNECTIONS.md` - Detailed wiring guide with schematics

## Troubleshooting
- **"ADS1115 not found"**: Check I2C wiring, verify STEMMA QT connection
- **Display garbled**: Ensure User_Setup.h is loaded by TFT_eSPI
- **WiFi won't connect**: Update SSID/password, check 2.4GHz network
- **NeoPixel not working**: GPIO 33 is built-in, no external wiring
- **Relay not clicking**: Check 5V power to relay module

## Version History
- **v2.0** (2025-11-03): Converted to ESP32-S2 TFT from LilyGo T-Display S3
- **v1.1**: Corrected version with improved error handling
- **v1.0**: Initial LilyGo version

## License
Open source - use and modify as needed for your sump monitoring application.
# SumpPumpMonitorController
