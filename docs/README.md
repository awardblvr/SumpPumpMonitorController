# Sump Monitor Controller
**ESP32-S2 Version for Adafruit Feather ESP32-S2 TFT**

## Overview
Automatic sump pump controller with water level monitoring, timeout protection, and alarm system.

## Hardware Requirements
- **Adafruit Feather ESP32-S2 TFT** (240x135 ST7789 display, built-in NeoPixel DISABLED)
- **ADS1115** 16-bit ADC module (I2C)
- **0-10V Water Level Sensor** with voltage divider (20kΩ, 10kΩ, 2.2µF cap - see QuickConnections.txt)
  - See Amazon for example: https://a.co/d/4JelvCt (QDY30A 0-10V 1M sensor)
- **5V Relay Module** (active-high) for pump control
- **2x Additional 5V Relay Modules** (GPIO 12, GPIO 13) - optional, not auto-controlled
- **Piezo Beeper**
- **2 Push Buttons** (NO, momentary)
- **2 GPIO Inputs** (GPIO 14/A4, GPIO 8/A5) with internal pullups
- **External NeoPixels** on GPIO 11 (2 pixels) - primary status indicator

## Features
- **Automatic Pump Control**: Turns pump on at 5" water level, off at 2"
- **No Sensor Offset**: Sensor offset set to 0.0" (adjust SENSOR_OFFSET_INCHES if needed)
- **No Averaging**: Uses current sensor reading directly (instant response, updates every 2 seconds)
- **Debug Control**: Serial output controlled by single flag (enaDbgSer: 0=silent, 1=verbose)
- **OTA Updates**: Wireless firmware updates via Arduino IDE (uses DHCP - recommend router reservation)
- **Timeout Protection**: 4-minute maximum runtime with pause/restart logic
- **Extension Run**: Pump runs 20 seconds after reaching off-level to ensure complete drain
- **High Water Alarm**: Triggers at 10" with beeper and flashing red LEDs
- **Visual Status**: External NeoPixels show system status (Green=OK, Red=Alarm, Blue=High Water)
- **Audible Alarm**: Beeper sounds if water level exceeds 10 inches
- **Additional Relays**: GPIO 12 and GPIO 13 available for manual relay control (not auto-controlled)
- **GPIO Monitoring**: Displays state of GPIO 14 and GPIO 8 inputs on screen
- **Historical Data**: Tracks pump runtime for last 5 days
- **WiFi Time Sync**: Timestamps pump runs via NTP (displays in 12-hour format with AM/PM)
- **WiFi Signal Strength**: Displays RSSI in dBm and current IP address on status screen
- **Manual Override**: Button for manual pump control
- **TFT Display**: Real-time water level, status, relay states, GPIO states, and statistics

## Quick Start
1. **Install Arduino Libraries**:
   - Adafruit_GFX
   - Adafruit_ST7789
   - Adafruit_ADS1X15
   - Adafruit_NeoPixel
   - WiFi (built-in)
   - Preferences (built-in)

2. **Wire Hardware**: See `feather_wiring_diagram.md` for complete wiring guide or `QuickConnections.txt` for quick reference

3. **Configure Sketch**:
   - Edit WiFi credentials in sketch (lines 50-51)
   - **Debug Output**: Set `enaDbgSer` flag (line 91): 0=no serial output, 1=enable debug output
   - **RECOMMENDED**: Set DHCP reservation in your router using the board's MAC address (shown on startup)
     - This ensures the board always gets the same IP for reliable OTA updates
     - See "Finding Your Board's MAC Address" section below
   - **Optional**: Use static IP instead of DHCP (uncomment lines 57-62 and configuration block)
   - Set OTA password (line 66): Change `ota_password` for security
   - Adjust thresholds if needed:
     - TRIGGER_INCHES=5.0 (pump turns ON)
     - OFF_INCHES=2.0 (pump turns OFF after 20-second extension)
     - SENSOR_OFFSET_INCHES=0.0 (adjust if sensor mounted above bottom)
     - TIMEOUT_SEC=240 (4-minute max runtime)
     - PAUSE_SEC=60 (1-minute pause after timeout)
     - EXTENSION_SEC=20 (20-second run after reaching OFF_INCHES)

4. **Upload**:
   - Select Board: **Adafruit Feather ESP32-S2 TFT**
   - **Configure board settings** (see Arduino IDE Board Settings section below)
   - First upload via USB: Upload `SumpMonitorController.ino`
   - After first upload: Use OTA for wireless updates (see OTA section below)

## Arduino IDE Board Settings

**CRITICAL**: These settings must be configured correctly for OTA to work.

### Required Settings (Tools Menu):

| Setting | Value | Notes |
|---------|-------|-------|
| **Board** | Adafruit Feather ESP32-S2 TFT | Exact board name |
| **Upload Speed** | 921600 | Fast; reduce to 460800 if errors occur |
| **USB CDC On Boot** | Enabled | **REQUIRED** for Serial.println() |
| **Partition Scheme** | Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS) | **⚠️ CRITICAL FOR OTA** |
| **CPU Frequency** | 240MHz (WiFi) | Maximum performance |
| **Flash Mode** | QIO | Fastest flash access |
| **Flash Frequency** | 80MHz | Maximum flash speed |
| **Flash Size** | 4MB (120KB SPIFFS) | Physical chip size |
| **Debug Level** | None | Use "Info" for troubleshooting |
| **Erase Flash** | Disabled | Only enable when needed |

### ⚠️ CRITICAL: Partition Scheme

**DO NOT use:** `TinyUF2 4MB (No OTA)` - This prevents OTA from working!

**Correct options:**
- `Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)` ✓ (Recommended)
- `Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)` ✓
- Any partition scheme with "OTA" in the name ✓

**If you previously uploaded with "No OTA" partition:**
1. Change partition scheme to one with OTA support
2. Upload via USB ONE MORE TIME to flash new partition table
3. After reboot, OTA will work wirelessly

### Port Selection:

**First Upload (USB):**
- Select: `/dev/cu.usbmodem*` (macOS) or `COM*` (Windows)

**After OTA Enabled (Network):**
- Select: `SumpMonitor at X.X.X.X` (network port)
- IP address shown during startup on Serial Monitor and TFT display

## Pin Assignments
| Function | GPIO | Notes |
|----------|------|-------|
| Relay (Pump) | 5 | Active-HIGH (corrected) |
| Beeper | 6 | Active-HIGH |
| Button 1 (Toggle) | 9 | Pullup, press=LOW |
| Button 2 (Reset) | 10 | Pullup, press=LOW |
| External NeoPixels | 11 | 2 pixels, primary status indicator |
| Relay 1 (K1) | 12 | Active-HIGH, manual control only |
| Relay 2 (K2) | 13 | Active-HIGH, manual control only |
| GPIO Input 1 (IO14) | 14 (A4) | Internal pullup, state shown on display |
| GPIO Input 2 (IO8) | 8 (A5) | Internal pullup, state shown on display |
| Built-in NeoPixel | 33 | 1 pixel, DISABLED |
| I2C SDA | 42 | STEMMA QT / ADS1115 |
| I2C SCL | 41 | STEMMA QT / ADS1115 |

## Operation
- **Green LED**: Normal operation, water level < 6"
- **Blue LED**: Water level 6-8" (approaching trigger point)
- **Purple LED**: Water level > 8" (pump should be running)
- **Red Flashing + Beeper**: ALARM - high water level (>10 inches)
- **External NeoPixels**: 2 pixels on GPIO 11 show all status indications
- **Built-in NeoPixel**: DISABLED (GPIO 33 not active)
- **Button 1**: Manual pump on/off toggle
- **Button 2**: Reset alarm (clears red LED and beeper)

**Display Status Line** (small cyan text):
```
K1:OFF K2:ON Problem?:n IO14:1 IO8:0
```
- **K1/K2**: Relay 1 and Relay 2 states (ON/OFF) - manual control only
- **Problem?**: High water alarm status (Y=alarm active, n=normal)
- **IO14/IO8**: GPIO input states (1=HIGH, 0=LOW)

![NeoPixel Status Indications](./Pump_Controller_Indications.jpg)

## OTA (Over-The-Air) Updates

After the initial USB upload, you can update the firmware wirelessly:

### Finding Your Board's MAC Address

The MAC address is displayed during startup:
- **Serial Monitor**: Shows `MAC Address: XX:XX:XX:XX:XX:XX`
- **TFT Display**: Shows `MAC: XX:XX:XX:XX:XX:XX`

**Setting up DHCP reservation** (RECOMMENDED for reliable OTA):
1. Upload sketch and note the MAC address from Serial Monitor or display
2. Note the IP address assigned by DHCP (also shown during startup)
3. Log into your router admin page (usually 192.168.1.1 or 192.168.0.1)
4. Find DHCP settings (may be called "Address Reservation", "Static DHCP", "IP Binding", etc.)
5. Create reservation: Bind the board's MAC address to a specific IP address
6. Save and reboot router if needed
7. The board will now always get the same IP via DHCP

**Benefits of DHCP reservation over static IP:**
- Centralized IP management in one place (your router)
- No need to edit code when changing IP addresses
- Router keeps track of all device IPs
- Works even if static IP config fails in code
- Best of both worlds: consistent IP + DHCP flexibility

### Using OTA Updates

1. **In Arduino IDE**:
   - Go to **Tools → Port**
   - Look for network port: **SumpMonitor at X.X.X.X** (IP shown during startup)
   - Select this network port

2. **Upload Sketch**:
   - Click Upload button (same as USB upload)
   - Enter OTA password when prompted (the `SECRET_OTA_PASSWORD` value in your local `secrets.h`)
   - Watch the TFT display show upload progress

3. **During Update**:
   - Display shows "OTA UPDATE" and progress percentage
   - Device automatically reboots when complete
   - Takes about 30-60 seconds total

4. **Troubleshooting OTA**:
   - **Port not showing**: Verify device is powered and connected to WiFi
   - **IP keeps changing**: Set up DHCP reservation in router (see MAC address section above)
   - **Password rejected**: Verify `ota_password` in sketch matches what you enter
   - **Update fails**: Try USB upload, check WiFi signal strength
   - **Can't find device**: Check Serial Monitor for actual IP address, verify mDNS is working on your network

**Security Note**: Change the default OTA password in the sketch (line 65) before deploying!

## Safety Notes
⚠️ **Important**:
- Pump must have separate power supply (DO NOT power from board)
- Use voltage divider for 10V sensor input (see `feather_wiring_diagram.md` section 3)
- Relay module needs 5V supply (USB or external)
- Test thoroughly before deploying in critical applications

## Files
- `SumpMonitorController.ino` - Main sketch (current version)
- `feather_wiring_diagram.md` - Detailed wiring guide with schematics
- `feather_setup_guide.md` - Complete setup instructions
- `QuickConnections.txt` - Quick connection reference

## Troubleshooting
- **"ADS1115 not found"**: Check I2C wiring (GPIO 42=SDA, 41=SCL), verify STEMMA QT connection
- **Display not working**: Check TFT_I2C_POWER (GPIO 21) and TFT_BACKLITE (GPIO 45) power
- **WiFi won't connect**: Update SSID/password (lines 50-51), check 2.4GHz network
- **IP address keeps changing**: Set up DHCP reservation in router using MAC address
- **No Serial output**: Check enaDbgSer flag (line 91), set to 1 to enable debug output
- **OTA not appearing in Arduino IDE**:
  - **Most common cause**: Partition scheme set to "No OTA" - change to "Default 4MB with spiffs" and upload via USB once
  - Verify WiFi connected and Serial Monitor shows "OTA ready"
  - Check router firewall settings (allow port 3232)
  - Ensure mDNS is working on your network
  - Verify password in code matches upload password (`SECRET_OTA_PASSWORD` in your local `secrets.h`)
- **OTA authentication fails**: Password mismatch - verify `ota_password` in sketch (line 66) matches upload password
- **External NeoPixels not working**: Wire to GPIO 11, check NEOPIXEL_POWER disabled built-in (GPIO 34 OFF), verify 2 pixels configured
- **Built-in NeoPixel still on**: Verify builtin_neo_present=false (line 46) and NEOPIXEL_POWER is OFF
- **Relay not clicking**: Check 5V power to relay module, verify active-HIGH logic
- **Pump runs backwards**: Relay logic is active-HIGH - HIGH=ON, LOW=OFF
- **Water level jumpy**: No averaging enabled - sensor reads directly every 2 seconds (instant response)

## Version History
- **v2.9** (2025-11-10): Major feature update
  - Added GPIO 12/13 relay outputs (manual control) and GPIO 14/8 inputs with pullups
  - Changed sensor offset to 0.0" (sensor now reads actual mounted position)
  - New pump logic: 4-minute timeout with 1-minute pause/restart, 20-second extension after reaching 2"
  - High water alarm at 10" with beeper and flashing red NeoPixels (hysteresis: clears at 9.5")
  - Added debug control flag (enaDbgSer) for conditional Serial output (0=silent, 1=verbose)
  - Changed sensor reading interval to 2 seconds
  - Disabled built-in NeoPixel completely (GPIO 34 power off)
  - Changed to single reading mode (no averaging) for instant water level response
  - Display status line shows relay states, alarm status, and GPIO input states
- **v2.8** (2025-11-10): Added comprehensive Arduino IDE board settings documentation for OTA support
- **v2.7** (2025-11-05): Added IP address display to WiFi status line (shows RSSI and IP)
- **v2.6** (2025-11-05): Changed from static IP to DHCP (recommend router DHCP reservation for OTA)
- **v2.5** (2025-11-05): Fixed external NeoPixels to 2 pixels on GPIO 11, built-in is 1 pixel, all track together
- **v2.4** (2025-11-05): Changed built-in NeoPixels to 2 pixels on GPIO 33, all pixels track together
- **v2.3** (2025-11-05): Added OTA update support with static IP configuration
- **v2.2** (2025-11-05): Added sensor offset correction (1.5"), changed OFF_INCHES to 2.0"
- **v2.1** (2025-11-04): Updated trigger levels (5"/3"), fixed relay logic (active-high), disabled built-in NeoPixel, added RSSI display, fixed date format to 12-hour AM/PM
- **v2.0** (2025-11-03): Converted to ESP32-S2 TFT from LilyGo T-Display S3
- **v1.1**: Corrected version with improved error handling
- **v1.0**: Initial LilyGo version

## License
Open source - use and modify as needed for your sump monitoring application.
# SumpPumpMonitorController
