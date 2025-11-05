# Sump Monitor Controller - Wiring Diagram
## Adafruit Feather ESP32-S2 TFT (Product #5300)

**CRITICAL: This uses CORRECT GPIO pins verified from official Adafruit board definition**

---

## Board Overview

The **Adafruit Feather ESP32-S2 TFT** features:
- ESP32-S2 microcontroller (single-core, 240MHz)
- Built-in **240x135** ST7789 TFT display
- Built-in NeoPixel RGB LED (GPIO 33)
- **I2C on GPIO 41 (SCL) and GPIO 42 (SDA)**
- 4MB Flash
- USB-C connector for power/programming
- LiPo battery charging circuit (JST connector)
- 21 GPIO pins on headers

---

## Critical Power Control Pins

⚠️ **MUST be enabled or components won't work!**

| GPIO | Function | Purpose | State |
|------|----------|---------|-------|
| **21** | **TFT_I2C_POWER** | Powers TFT display AND I2C pullups | **MUST be HIGH** |
| **34** | **NEOPIXEL_POWER** | Powers built-in NeoPixel LED | **MUST be HIGH** |
| **45** | **TFT_BACKLITE** | TFT backlight control | HIGH = on |

**Without GPIO 21 HIGH:**
- TFT display won't work
- I2C communication won't work (no pullups)
- ADS1115 won't be detected

**Without GPIO 34 HIGH:**
- NeoPixel LED won't light up

---

## Complete Pinout Diagram

```
                    Adafruit Feather ESP32-S2 TFT
                          (TOP View)

                 USB-C                              
                   ||                                    
            ┌──────┴┴──────────────────────────────────┐┌─┐
            │  ┌──────────────┐                        ││ │Reset Button
            │  │ TFT 240x135  │ TFT I2C POWER: GPIO21  │└─┘
            │  │   Display    │ TFT RESET: GPIO40      │
            │  │   ST7789     │ TFT BACKLIGHT: GPI045  │
            │  └──────────────┘ TFT CS: 7, DC: 39      │
            │                                          │
            │                                          │
            │  NeoPixel LED (data:GPIO33               │
            │                power: GPIO34)            │
            │  Left Side Pins:     Right Side Pins:    │
            │  ───────────────     ────────────────    │
            │  RESET                                   │
            │  3V   (3.3V out)      (batt conn: GND)   │
            │  3V   (3.3V out)      (batt conn: VBAT)  │
            │  GND                                     │
            │  A0   (GPIO18)       VBAT                │
            │  A1   (GPIO17)       EN                  │
            │  A2   (GPIO16)       VBUS                │
            │  A3   (GPIO15)       D13  (GPIO13)       │
            │  A4   (GPIO14)       D12  (GPIO12)       │
            │  A5   (GPIO8)        D11  (GPIO11) ◄──External NeoPixel
shared DISP │  SCK  (GPIO36)       D10  (GPIO10) ◄──Button 2 (Alarm)
shared DISP │  MOSI (GPIO35)       D9   (GPIO9)  ◄──Button 1 (Pump)
shared DISP │  MISO (GPIO37)       D6   (GPIO6)  ◄──Beeper
            │  Rx   (GPIO2)        D5   (GPIO5)  ◄──Relay
            │  Tx   (GPIO1)        SCL  (GPIO41) ◄──I2C Clock (ADS1115)
            │  TxD0 (Debug)        SDA  (GPIO42) ◄──I2C Data (ADS1115)
            └───────────────────────────────────────────┘

    
```

**Key Internal Pins (not on headers):**
- GPIO 21: TFT_I2C_POWER (must be HIGH)
- GPIO 34: NEOPIXEL_POWER (must be HIGH)
- GPIO 33: NeoPixel data
- GPIO 45: TFT backlight
- GPIO 7, 35-40: TFT SPI (internal)

---

## Component Connections

### 1. ADS1115 ADC Module (I2C - 16-bit Analog to Digital Converter)

**Pin Connections:**
```
ADS1115 Module          Feather ESP32-S2 TFT
──────────────          ────────────────────
VDD   (Power)     ────► 3V    (3.3V power - LEFT side header)
GND   (Ground)    ────► GND   (Ground - LEFT side header)
SCL   (I2C Clock) ────► GPIO 41 (SCL - RIGHT side header)
SDA   (I2C Data)  ────► GPIO 42 (SDA - RIGHT side header)
ADDR  (Address)   ────► GND   (Sets I2C address to 0x48)
A0    (Analog 0)  ────► [Voltage divider output - see below]
A1-A3 (unused)
```

**I2C Address:**
- ADDR pin to GND = Address 0x48 (default)
- ADDR pin to VDD = Address 0x49
- ADDR pin to SDA = Address 0x4A
- ADDR pin to SCL = Address 0x4B

**I2C Pullup Resistors:**
- ✅ **NOT NEEDED** - Built-in pullups enabled when GPIO 21 is HIGH
- ADS1115 module has onboard 10kΩ pullups
- Feather has pullups powered by TFT_I2C_POWER (GPIO 21)
- **Do NOT add external pullups** - may cause issues

---

### 2. 12V Pressure Sensor Power Supply (Buck Converter)

**3.3V-to-12V Step-Up Converter:**

The pressure sensor requires 12V power, which we generate from the Feather's 3.3V output using a small buck/boost converter module.

```
Buck Converter Module (3.3V → 12V)
┌────────────────────────┐
│  Vin  ──◄ 3.3V         │  Current draw: ~5-10mA from 3.3V
│  GND  ──◄ GND          │  Output: 12V @ up to 100mA
│  Vout ──► 12V          │
└────────────────────────┘
         │
         ├─────► Pressure Sensor +12V (Red wire)
         │
        GND ────► Pressure Sensor GND (Black wire)
```

**Pin Connections:**
```
Buck Converter          Feather ESP32-S2 TFT
──────────────          ────────────────────
Vin  (Input 3.3V) ────► 3V    (3.3V power - LEFT side header)
GND  (Ground)     ────► GND   (Ground)
Vout (Output 12V) ────► Pressure Sensor +12V (Red wire)
```

**Recommended Buck Converter Modules:**
- **Pololu U3V12F12** - 3.3V to 12V step-up voltage regulator
- **XL6009 module** - Adjustable DC-DC boost converter (set to 12V)
- **MT3608 module** - 2A step-up boost converter (adjust to 12V)

**Current Budget Check:**
- Buck converter input current: ~5-10 mA @ 3.3V
- Pressure sensor current: ~10-30 mA @ 12V
- Total 3.3V load from converter: ~5-10 mA (acceptable within 280mA thermal limit)

**3-Wire Pressure Sensor Connections:**
```
Pressure Sensor         Connection
───────────────         ──────────
Black (GND)       ────► Common GND (shared with Feather)
Red   (+12V)      ────► Buck converter 12V output
Yellow (Signal)   ────► Voltage divider input (see next section)
                        (Outputs 0-10V proportional to water level)
```

**Sensor Specifications:**
- Power: 12V DC ±10%
- Current: 10-30 mA typical
- Output: 0-10V (proportional to 0-1 meter water depth)
- Accuracy: Typically ±0.5%

**Example Sensor:**
- QDY30A 0-10V 1M Liquid Level Sensor
- See Amazon: https://a.co/d/4JelvCt
- See detailed specifications in `QDY30A_0-10V_1M_Liquid_Level_Sensor_datasheet.md`

---

### 3. Voltage Divider Circuit (0-10V Sensor → 0-3.3V for ADS1115)

**Required for 0-10V water level sensors!**

```
Water Sensor           Voltage Divider         ADS1115
(0-10V Output)                                 A0 Input
     │
     │
  Sensor OUT ─────[20kΩ]──┬──[10kΩ]──► GND
                          │
                          ├──────────► ADS1115 A0
                          │
                          └── 2.2µF──► GND
                              (+ to signal, - to GND)

  Sensor GND ─────────────────────────► Common GND
```

**Component List (AS IMPLEMENTED):**
- R1: 20kΩ resistor (1% tolerance, 1/4W, metal film)
- R2: 10kΩ resistor (1% tolerance, 1/4W, metal film)
- C: 2.2µF electrolytic capacitor (observe polarity! + to signal, - to GND)

**Note on Alternative Designs:**
Some designs include a 100Ω series resistor between the divider and ADC input, plus additional capacitors (10µF + 0.1µF). The 100Ω resistor forms an RC low-pass filter with the capacitors to reduce high-frequency noise from pump motors or switching power supplies. The current implementation uses a simpler single 2.2µF capacitor which provides adequate filtering for this application and has been verified working.

**Voltage Calculation:**
- Divider ratio: R2 / (R1 + R2) = 10kΩ / 30kΩ = 0.333
- Input 0V → Output 0V
- Input 10V → Output 3.33V (safe for ADS1115 max 3.3V input)

**Safety:**
- ADS1115 maximum input voltage: ±(VDD + 0.3V) = 3.6V
- Our max output: 3.33V ✓ SAFE
- Even if sensor outputs 12V: 12V × 0.333 = 4.0V (still within ADS1115 abs max rating)

---

### 4. Relay Module (5V, Active-High for Pump Control - CORRECTED)

**Pin Connections:**
```
5V Relay Module         Feather ESP32-S2 TFT
───────────────         ────────────────────
VCC   (Power 5V)  ────► USB  (5V from USB - RIGHT side)
GND   (Ground)    ────► GND  (Ground)
IN    (Control)   ────► GPIO 5  (RIGHT side header)
```

**Relay Terminals (High Voltage - AC Pump):**
```
Relay Screw Terminals    Pump Wiring
─────────────────────    ────────────
COM  (Common)      ────► AC HOT (from breaker/switch)
NO   (Normally Open) ──► Pump HOT wire
NC   (Not used)          [Leave disconnected]

Pump NEUTRAL wire  ────► AC NEUTRAL (direct, no relay)
Pump GROUND wire   ────► AC GROUND/Earth
```

**⚠️ HIGH VOLTAGE WARNINGS:**
- 120VAC/240VAC is **LETHAL** - can kill instantly
- **Hire qualified electrician** for all AC wiring
- Install **GFCI protection** (required by code)
- Install **circuit breaker** for overcurrent protection
- Follow **NEC** (National Electrical Code) and local codes
- Use proper gauge wire (14AWG minimum for 15A circuit)
- Ensure proper grounding
- Test with multimeter before energizing
- **NEVER** work on live AC circuits

**Active-High Logic (CORRECTED v2.1):**
- GPIO 5 = HIGH (3.3V) → Relay ON → Pump ON
- GPIO 5 = LOW (0V) → Relay OFF → Pump OFF

**Note:** Earlier versions used active-low logic. This was corrected in v2.1 due to relay hardware behavior.

---

### 4. Piezo Beeper (Alarm Sound)

**Pin Connections:**
```
Piezo Beeper            Feather ESP32-S2 TFT
────────────            ────────────────────
+  (Positive)     ────► GPIO 6  (RIGHT side header)
-  (Negative/GND) ────► GND     (Ground)
```

**Types:**
- **Active Buzzer**: Sounds when powered (simpler)
- **Passive Buzzer**: Requires PWM frequency (code uses simple on/off)

**Recommended:**
- 3-5V active piezo buzzer
- Current draw: ~20-30mA typical
- Loud enough to hear from distance

---

### 5. Push Buttons (Manual Control)

**Button 1 - Manual Pump Toggle (GPIO 9):**
```
Momentary Switch        Feather ESP32-S2 TFT
────────────────        ────────────────────
Terminal 1        ────► GPIO 9  (RIGHT side header)
Terminal 2        ────► GND     (Ground)
```

**Button 2 - Alarm Reset (GPIO 10):**
```
Momentary Switch        Feather ESP32-S2 TFT
────────────────        ────────────────────
Terminal 1        ────► GPIO 10 (RIGHT side header)
Terminal 2        ────► GND     (Ground)
```

**Internal Pullups:**
- ✅ Code enables internal pullup resistors
- No external resistors needed
- Buttons are **active-low** (pressed = LOW signal)
- Use **normally-open momentary** push buttons
- Debouncing handled in code (500ms)

---

### 6. External NeoPixel LED (Optional Status Indicator)

**Optional external WS2812B RGB LED for additional visual feedback**

⚠️ **Important Notes (v2.1+):**
- The built-in NeoPixel (GPIO 33) is **DISABLED** in code (brightness set to 0)
- External NeoPixel on GPIO 11 is used as the primary status indicator
- This reduces onboard LED glare while maintaining clear visual status feedback

**Pin Connections with Protection Components:**

```
                         External NeoPixel WS2812B
Feather ESP32-S2 TFT
                         ┌────────────────┐
GPIO 11 ────[330Ω]──────►│ DIN (Data In)  │
                         │                │
5V (USB pin) ────────────►│ VCC / +5V      │
      │                  │                │
      │                  │ GND            │───► GND
      │                  └────────────────┘
      │                       │
      │                       │
     ═╪═ C3 = 2.2µF           │
      │                       │
      │   16V+ Electrolytic   │
      │   + to 5V rail        │
      │                       │
     GND ─────────────────────┘
```

**Component List:**
- R_data: 330Ω resistor (1/4W, between GPIO 11 and NeoPixel DIN)
- C3: 2.2µF electrolytic capacitor (16V+, across NeoPixel power)
- NeoPixel: WS2812B single LED or strip

**Detailed Connections:**
```
Component/Pin           Feather ESP32-S2 TFT
─────────────           ────────────────────
NeoPixel DIN      ────► GPIO 11 (RIGHT side) via 330Ω resistor
NeoPixel VCC/+5V  ────► USB (5V - RIGHT side header)
NeoPixel GND      ────► GND (Ground)

Capacitor (2.2µF):
  + terminal      ────► 5V rail (near NeoPixel)
  - terminal      ────► GND (near NeoPixel)
```

**Why These Components?**

**330Ω Series Resistor (R_data):**
- Protects GPIO from voltage spikes
- Reduces signal ringing and reflections
- Limits inrush current to NeoPixel data pin
- Can use 220-470Ω range

**2.2µF Decoupling Capacitor (C3):**

- Provides current surge during color changes
- Stabilizes 5V power supply
- Reduces voltage drops when LED brightness changes
- Must be rated for 16V or higher
- **Observe polarity!** + to 5V, - to GND
- Place physically close to NeoPixel power pins

**Power Requirements:**
- Single WS2812B LED: up to 60mA @ full white (255,255,255)
- Typical use (colors, dimmed): 10-30mA
- Safe for 5V USB power supply

**Code Initialization:**
```cpp
#include <Adafruit_NeoPixel.h>

#define EXTERNAL_NEO_PIN 11  // GPIO 11 for external NeoPixel
#define EXTERNAL_NEO_COUNT 1 // Number of LEDs in strip/ring

Adafruit_NeoPixel externalPixel(EXTERNAL_NEO_COUNT, EXTERNAL_NEO_PIN,
                                  NEO_GRB + NEO_KHZ800);

void setup() {
  externalPixel.begin();
  externalPixel.setBrightness(50);  // 0-255, start at 50 for safety
  externalPixel.setPixelColor(0, externalPixel.Color(0, 255, 0)); // Green
  externalPixel.show();
}
```

**NeoPixel Strip/Ring:**
If using multiple NeoPixels (strip or ring):
- Increase `EXTERNAL_NEO_COUNT` to match LED quantity
- Increase C3 capacitance: Use 1000µF per 50-60 LEDs
- Verify 5V power supply can handle total current (60mA × LED count)
- For >10 LEDs, consider external 5V power supply

**Mounting Options:**
- Panel-mount 8mm NeoPixel LED
- Adafruit NeoPixel ring (12 or 16 LEDs)
- NeoPixel strip (cut to length)

---

### 7. Built-in Components (No External Wiring)

**NeoPixel RGB LED:**
- GPIO: 33 (data)
- GPIO 34: NEOPIXEL_POWER (must be HIGH)
- Location: On back of board
- Quantity: 1 LED
- Type: WS2812B or compatible
- Status indicator with color coding

**TFT Display:**
- Type: ST7789
- Resolution: 240x135 pixels
- Interface: SPI (internal connections)
- GPIO 21: TFT_I2C_POWER (must be HIGH)
- GPIO 45: TFT_BACKLITE (backlight control)
- No external wiring needed

---

## GPIO Summary Table

| GPIO | Direction | Function | Connected To | Notes |
|------|-----------|----------|--------------|-------|
| 1 | I/O | TX (UART) | (available) | |
| 2 | I/O | RX (UART) | (available) | |
| 3 | I/O | General | (available) | |
| 4 | I/O | General | (available) | |
| **5** | **Output** | **Relay Control** | **Relay IN** | Active-low |
| **6** | **Output** | **Beeper** | **Piezo +** | |
| 7 | Output | TFT_CS | TFT (internal) | |
| 8 | I/O | A5 | (available) | |
| **9** | **Input** | **Button 1** | **Toggle button** | Pullup enabled |
| **10** | **Input** | **Button 2** | **Alarm button** | Pullup enabled |
| **11** | **Output** | **External NeoPixel** | **WS2812B data** | Optional, via 330Ω resistor |
| 12-13 | I/O | General | (available) | |
| 14 | I/O | A4 | (available) | |
| 15 | I/O | A3 | (available) | |
| 16 | I/O | A2 | (available) | |
| 17 | I/O | A1 / DAC1 | (available) | |
| 18 | I/O | A0 / DAC2 | (available) | |
| **21** | **Output** | **TFT_I2C_POWER** | **TFT & I2C power** | **MUST be HIGH** |
| **33** | **Output** | **NeoPixel** | **Built-in LED** | Internal |
| **34** | **Output** | **NEOPIXEL_POWER** | **NeoPixel power** | **MUST be HIGH** |
| 35 | Output | TFT_MOSI | TFT (internal) | |
| 36 | Output | TFT_SCLK | TFT (internal) | |
| 37 | Input | TFT_MISO | TFT (internal) | |
| 38 | I/O | General | (available) | |
| 39 | Output | TFT_DC | TFT (internal) | |
| 40 | Output | TFT_RST | TFT (internal) | |
| **41** | **Bidir** | **I2C SCL** | **ADS1115 SCL** | Clock |
| **42** | **Bidir** | **I2C SDA** | **ADS1115 SDA** | Data |
| **45** | **Output** | **TFT_BACKLITE** | **TFT backlight** | HIGH = on |

**Available GPIOs:** 1-4, 8, 12-18, 38 (GPIO 11 used for optional external NeoPixel)

---

## Power Distribution

```
External 5V Power Supply (via VBUS pin or USB-C)
(5V / 2A minimum recommended)
      │
      ├──► VBUS/USB-C ──────────► Feather ESP32-S2 TFT Board
      │                              │
      │                              ├─► 3.3V Regulator (AP2112K) ──► 3V pin
      │                              │   (500mA max, 280mA thermal limit)
      │                              │                      │
      │                              │                      ├──► ADS1115 VDD (1mA)
      │                              │                      │
      │                              │                      ├──► Buck Converter Vin (5-10mA)
      │                              │                      │         │
      │                              │                      │         └──► 12V out ──► Pressure Sensor +12V
      │                              │                      │
      │                              │                      ├──► ESP32-S2 & TFT (150-220mA)
      │                              │                      └──► Button pullups
      │                              │
      │                              ├─► 5V (USB pin) ─────┬──► Relay VCC (70mA when on)
      │                              │                     │
      │                              │                     ├──► External NeoPixel VCC (10-60mA)
      │                              │                     │     via 1000µF decoupling cap
      │                              │                     │
      │                              │                     └──► (Available for other 5V loads)
      │                              │
      │                              ├─► GPIO 21 HIGH ────► TFT & I2C pullup power
      │                              │
      │                              ├─► GPIO 34 HIGH ────► Built-in NeoPixel power
      │                              │
      │                              └─► Common GND ──────┬──► ADS1115 GND
      │                                                    ├──► Relay GND
      │                                                    ├──► Beeper GND
      │                                                    ├──► Buttons GND
      │                                                    ├──► Buck Converter GND
      │                                                    ├──► Pressure Sensor GND
      │                                                    ├──► External NeoPixel GND
      │                                                    └──► Voltage divider GND
      │
      └──► (Optional) Battery JST ──► 3.7V LiPo (backup power, 500-2000mAh)
```

**3.3V Current Budget (Thermal Limited):**
| Load | Current | Running Total |
|------|---------|---------------|
| ESP32-S2 (WiFi active) | 150-180 mA | 150-180 mA |
| TFT Display controller | 10 mA | 160-190 mA |
| ADS1115 | 1 mA | 161-191 mA |
| Buck Converter | 5-10 mA | 166-201 mA |
| **Total 3.3V** | **166-201 mA** | ✓ Within 280mA thermal limit |
| **Available for expansion** | **79-114 mA** | |

**5V Current Budget:**
| Load | Current | Running Total |
|------|---------|---------------|
| TFT Backlight | 40-60 mA | 40-60 mA |
| Relay module (coil on) | 70 mA | 110-130 mA |
| External NeoPixel (single) | 10-60 mA | 120-190 mA |
| Beeper (when sounding) | 30 mA | 150-220 mA |
| **Total 5V** | **150-220 mA** | |
| **Total system (3.3V + 5V)** | **316-421 mA** | ✓ Within 2A PSU rating |

**Power Consumption (Updated):**
| Component | Current Draw | Power Supply | Notes |
|-----------|--------------|--------------|-------|
| ESP32-S2 (WiFi active) | ~150-180mA | 3.3V | Peak during TX |
| TFT Display controller | ~10mA | 3.3V | Logic only |
| TFT Backlight | ~40-60mA | 5V | Brightness dependent |
| Built-in NeoPixel | ~20mA | 5V (internal) | At 50% brightness |
| External NeoPixel (optional) | ~10-60mA | 5V | Single LED, color dependent |
| ADS1115 | ~1mA | 3.3V | Continuous mode |
| Buck Converter | ~5-10mA | 3.3V | Input current |
| Pressure Sensor | ~10-30mA | 12V | From buck converter |
| Relay coil | ~70mA | 5V | When energized |
| Beeper | ~30mA | 3.3V | When sounding |
| **Total (all on)** | **~316-471mA** | **5V input** | Worst case |
| **Typical (idle)** | **~200-250mA** | **5V input** | Normal operation |
| **Recommended PSU** | **5V / 2A** | | 2000mA = plenty of headroom |

---

## Power Filtering and Decoupling Capacitors

**IMPORTANT:** The following capacitors are installed in the actual implementation for power supply stability and noise reduction:

### Main Power Input Filtering
- **100µF electrolytic capacitor** across main DC input (VBUS/5V and GND)
  - Location: At USB power input
  - Purpose: Bulk filtering, absorbs current spikes from relay/pump switching
  - Polarity: + to 5V, - to GND

### Module Decoupling (0.1µF ceramic capacitors)
Install **0.1µF ceramic capacitors** between VCC and GND on all modules:

1. **Feather ESP32-S2 TFT module**
   - Location: Across 3.3V and GND pins near the module
   - Purpose: High-frequency noise filtering for CPU and WiFi operation

2. **ADS1115 ADC module**
   - Location: Across VDD and GND pins at the module
   - Purpose: Reduce ADC reading noise from digital switching

3. **Buck-Boost Converter (input side)**
   - Location: Across 5V input and GND at converter input
   - Purpose: Filter switching noise from buck converter operation

### Additional Power Filtering (2.2µF capacitors)

1. **External NeoPixel power input**
   - **2.2µF capacitor** across VCC and GND at NeoPixel
   - Purpose: Prevent voltage droops during LED color changes
   - Polarity: + to 5V, - to GND

2. **Buck-Boost Converter input**
   - **2.2µF capacitor** across converter input (3.3V and GND)
   - Purpose: Smooth input voltage for stable 12V output
   - Polarity: + to 3.3V, - to GND

3. **Buck-Boost Converter output**
   - **2.2µF capacitor** across converter output (12V and GND)
   - Purpose: Filter 12V output for clean sensor power
   - Polarity: + to 12V, - to GND

### Capacitor Summary Table

| Location | Type | Value | Purpose |
|----------|------|-------|---------|
| Main 5V input | Electrolytic | 100µF | Bulk filtering, absorbs switching spikes |
| Feather module | Ceramic | 0.1µF | High-frequency decoupling |
| ADS1115 module | Ceramic | 0.1µF | ADC noise reduction |
| Buck converter input (5V side) | Ceramic | 0.1µF | Switching noise filtering |
| NeoPixel power | Electrolytic | 2.2µF | LED current spike filtering |
| Buck converter input (3.3V side) | Electrolytic | 2.2µF | Input smoothing |
| Buck converter output (12V) | Electrolytic | 2.2µF | Output ripple reduction |

**Notes:**
- All electrolytic capacitors must observe correct polarity
- Ceramic capacitors (0.1µF) are non-polarized
- Place capacitors as close as possible to the power pins they're protecting
- These capacitors are critical for stable operation, especially with relay switching and WiFi transmission

---

## Bill of Materials (BOM)

| Item | Quantity | Specifications | Approx Price |
|------|----------|----------------|--------------|
| **Main Components** | | | |
| Adafruit Feather ESP32-S2 TFT | 1 | Product #5300 | $25 |
| ADS1115 ADC Module | 1 | 16-bit I2C, 0x48 default address | $10 |
| 12V Pressure sensor | 1 | 0-10V output, 0-1m range, 3-wire (e.g., QDY30A - see https://a.co/d/4JelvCt) | $30-80 |
| Buck/Boost Converter | 1 | 3.3V→12V step-up (Pololu U3V12F12 or similar) | $6-10 |
| 5V Relay module | 1 | Active-low, 10A contacts minimum | $5 |
| Piezo buzzer | 1 | Active, 3-5V, loud | $2 |
| Push buttons | 2 | Momentary, normally-open | $1 |
| **Voltage Divider Components** | | | |
| 20kΩ resistor (1%) | 1 | 1/4W, metal film | $0.10 |
| 10kΩ resistor (1%) | 1 | 1/4W, metal film | $0.10 |
| 2.2µF electrolytic cap | 1 | 16V+ rating, for sensor signal filtering | $0.15 |
| **Power Filtering Capacitors** | | | |
| 100µF electrolytic cap | 1 | 16V+ rating, main power input filtering | $0.25 |
| 0.1µF ceramic cap | 3 | 50V rating, module decoupling (Feather, ADS1115, Buck converter) | $0.15 |
| 2.2µF electrolytic cap | 3 | 16V+ rating, power filtering (NeoPixel, Buck in/out) | $0.45 |
| **External NeoPixel (Optional)** | | | |
| WS2812B NeoPixel LED | 1 | Single 8mm through-hole or SMD module | $2-5 |
| 330Ω resistor | 1 | 1/4W, data line protection | $0.05 |
| 1000µF electrolytic cap | 1 | 16V+ rating, power decoupling | $0.50 |
| **Wiring and Enclosure** | | | |
| Breadboard/protoboard | 1 | For voltage divider | $3-10 |
| Jumper wires | Assorted | Male-female, male-male | $5 |
| USB-C cable | 1 | For power and programming | $5 |
| 5V/2A power supply | 1 | Wall adapter with USB or VBUS connection | $8-12 |
| Waterproof enclosure | 1 | IP65 rated | $15-30 |
| **Total (without optional NeoPixel)** | | | **~$120-190** |
| **Total (with optional NeoPixel)** | | | **~$123-195** |

---

## Assembly Order

1. **Build voltage divider circuit** on breadboard/protoboard (20kΩ, 10kΩ, 2.2µF cap)
2. **Test voltage divider** with multimeter (10V in → 3.33V out)
3. **Connect buck converter** to Feather 3.3V pin
4. **Adjust buck converter** output to 12V (use multimeter to verify)
5. **Connect pressure sensor** power (GND to common, +12V to buck output)
6. **Connect sensor signal** to voltage divider input
7. **Connect ADS1115** to Feather (VDD to 3V, GND, SCL=GPIO41, SDA=GPIO42, ADDR=GND)
8. **Connect voltage divider output** to ADS1115 A0
9. **Upload test sketch** to verify I2C communication and sensor readings
10. **Connect relay module** (VCC to USB/5V, GND, IN to GPIO5)
11. **Connect beeper** (+ to GPIO6, - to GND)
12. **Connect buttons** (GPIO9 and GPIO10 to GND)
13. **(Optional) Connect external NeoPixel:**
    - Solder 330Ω resistor between GPIO11 and NeoPixel DIN
    - Connect 1000µF cap across NeoPixel power (+ to 5V, - to GND)
    - Connect NeoPixel VCC to USB/5V, GND to common ground
14. **Upload full sketch** and test all functions (display, buttons, relay, beeper, LEDs)
15. **Verify sensor readings** match expected water levels
16. **Wire pump to relay AC contacts** (⚠️ GET QUALIFIED ELECTRICIAN FOR HIGH VOLTAGE WIRING!)
17. **Install all components in weatherproof enclosure**
18. **Deploy and test** in sump pit location
19. **Monitor for 24 hours** to verify stable operation

---

## Testing Checklist

### Pre-Power Tests
- [ ] Visual inspection of all connections
- [ ] Continuity test: all GNDs common
- [ ] No shorts between 3.3V/5V and GND
- [ ] Voltage divider output measures correct (3.33V at 10V input)

### Initial Power-On (Code Required)
- [ ] Buck converter outputs 12V (adjust trimmer pot if needed)
- [ ] Pressure sensor has 12V power
- [ ] I2C scanner detects ADS1115 at address 0x48
- [ ] Display shows initialization messages
- [ ] Built-in NeoPixel stays OFF (disabled in v2.1+)
- [ ] External NeoPixel lights up (shows status)
- [ ] WiFi connects and shows IP address
- [ ] Water level readings appear on display

### Functional Tests
- [ ] Pressure sensor readings change with voltage input (0-10V test)
- [ ] Button 1 (GPIO 9) toggles relay (hear audible click)
- [ ] Button 2 (GPIO 10) resets alarm (when alarm active)
- [ ] Beeper sounds during alarm condition
- [ ] Relay contacts switch properly (test with multimeter)
- [ ] Serial monitor shows debug messages every 10 seconds

### Safety Tests (HIGH VOLTAGE - ELECTRICIAN REQUIRED)
- [ ] All AC wiring meets local electrical codes
- [ ] GFCI protection installed on pump circuit
- [ ] Circuit breaker sized correctly for pump load
- [ ] AC ground/earth connected properly
- [ ] Relay contacts rated for pump current
- [ ] No exposed AC conductors
- [ ] Enclosure is properly grounded

### Long-Term Monitoring
- [ ] System runs stable for 24 hours minimum
- [ ] No thermal issues (feel enclosure/board for excessive heat)
- [ ] WiFi remains connected
- [ ] Sensor readings are stable and consistent
- [ ] Pump cycles correctly based on water level

---

**Document Version:** 3.1 (Updated for v2.1 firmware: corrected relay logic, disabled built-in NeoPixel)
**Last Updated:** 2025-11-04
**Board:** Adafruit Feather ESP32-S2 TFT (Product #5300)
**Major Changes:**
- v3.1 (2025-11-04): Corrected relay logic to active-high, noted built-in NeoPixel disabled
- v3.0 (2025-11-03): Added 12V sensor power supply, external NeoPixel, pressure sensor details
- v2.0 (2025-11-03): Converted from LilyGo T-Display S3 to Feather ESP32-S2 TFT
- v1.0: Initial LilyGo version
- Added 3.3V→12V buck converter for pressure sensor
- Added external NeoPixel LED with proper protection components (330Ω resistor + 1000µF cap)
- Updated power distribution with current budgets
- Updated BOM with new components
- Corrected GPIO assignments (I2C on GPIO 41/42, not 3/4)
