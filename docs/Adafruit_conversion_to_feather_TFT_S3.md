# Adafruit ESP32-S2 to ESP32-S3 TFT Feather Conversion Analysis

## Summary

The ESP32-S2 TFT Feather and ESP32-S3 TFT Feather have similar form
factors, but **different internal GPIO mappings**. Several GPIOs
available on the S2 are **not usable** on the S3 due to being reserved
for the SPI flash interface.

This document lists which pins must be changed, recommended
replacements, and required software adjustments.

------------------------------------------------------------------------

## 1. GPIO Compatibility Overview

The following GPIOs you currently use on the ESP32-S2 **cannot** be used
on the ESP32-S3:

-   **GPIO 5**
-   **GPIO 6**
-   **GPIO 8**
-   **GPIO 9**
-   **GPIO 10**
-   **GPIO 11**
-   **GPIO 12**

These are missing or reserved for flash on the S3.

The following **are usable** on both boards:

-   **GPIO 13**
-   **GPIO 14**
-   **GPIO 41 (I2C SCL)**
-   **GPIO 42 (I2C SDA)**

------------------------------------------------------------------------

## 2. Required Remapping (Recommended)

  Function             Old (S2)   New (S3 Recommended)
  -------------------- ---------- -------------------------
  Pump relay           GPIO 5     **GPIO 33**
  Beeper               GPIO 6     **GPIO 34**
  Misc input 2         GPIO 8     **GPIO 35**
  Button 1             GPIO 9     **GPIO 36**
  Button 2             GPIO 10    **GPIO 37**
  External NeoPixels   GPIO 11    **GPIO 38**
  Relay 1              GPIO 12    **GPIO 40**
  Relay 2              GPIO 13    **GPIO 13** (unchanged)
  Misc input 1         GPIO 14    **GPIO 14** (unchanged)
  I2C SCL              GPIO 41    **GPIO 41**
  I2C SDA              GPIO 42    **GPIO 42**

------------------------------------------------------------------------

## 3. Code Changes (Arduino Example)

Update your pin definitions only:

``` cpp
#define PUMP_RELAY_PIN     33
#define BEEPER_PIN         34
#define MISC_IN2_PIN       35
#define BUTTON1_PIN        36
#define BUTTON2_PIN        37
#define NEOPIXEL_PIN       38
#define RELAY1_PIN         40
#define RELAY2_PIN         13
#define MISC_IN1_PIN       14

#define I2C_SDA_PIN        42
#define I2C_SCL_PIN        41
```

Everything else (digitalWrite, pinMode, Wire, NeoPixel library) works
the same.

------------------------------------------------------------------------

## 4. Notes About ESP32-S3 GPIO Restrictions

-   GPIO **6--12** are permanently tied to the onboard flash → **not
    usable**.
-   GPIO **1--4** exist but are used internally depending on boot
    configuration.
-   Higher GPIOs (33--40) are safe and exposed on the TFT Feather.

------------------------------------------------------------------------

## 5. Conclusion

**You cannot keep the same wiring** when switching from ESP32-S2 TFT
Feather to ESP32-S3 TFT Feather.

However, the S3 has abundant alternate pins, and by remapping to the
recommended GPIOs you can reuse your PCB or wiring harness with only
minimal changes.

If you want, I can produce: - A KiCad pinout sheet\
- A schematic showing S2 vs S3 mapping\
- A PlatformIO version of the configuration
