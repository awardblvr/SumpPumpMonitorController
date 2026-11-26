# QDY30A Liquid Level Transmitter Specification Sheet

## 1. Introduction

The QDY30A Liquid Level Sensor Transmitter is a high-precision device designed for continuous and accurate measurement of liquid levels, particularly in water tanks. Utilizing hydrostatic pressure principles, it converts the measured pressure into a standard electrical output signal (4-20mA, 0-10V, or others like RS485 in variants), making it suitable for various industrial and domestic monitoring applications. Its robust 304 stainless steel construction and IP68 protection ensure reliable performance in demanding environments.

Figure 1: Two QDY30A Liquid Level Sensors. This image displays two QDY30A liquid level sensors, highlighting their robust stainless steel construction and the integrated cable for electrical connection. The sensors are designed for precise hydrostatic level measurement.




## 2. Safety Information

Please read and understand all safety instructions before installing or operating the QDY30A Liquid Level Sensor. Failure to follow these instructions may result in equipment damage, personal injury, or inaccurate readings.

- Power Supply: Ensure the power supply voltage (12-36VDC) matches the sensor's requirements. Incorrect voltage can damage the device.
- Wiring: All wiring should be performed by qualified personnel in accordance with local electrical codes. Ensure proper polarity for DC connections.
- Environment: Although the sensor has an IP68 rating, avoid exposing the cable connections to excessive moisture or corrosive substances unless properly sealed.
- Handling: Handle the sensor carefully. Do not drop or subject it to severe mechanical shock, as this may affect its accuracy and integrity.
- Maintenance: Disconnect power before performing any maintenance or cleaning.

## 3. Product Overview

The QDY30A sensor consists of a robust stainless steel probe, an integrated cable, and internal electronics for signal conditioning. The probe is designed to be submerged in the liquid, where it measures the hydrostatic pressure, which is directly proportional to the liquid level above the sensor.

Figure 2: QDY30A Sensor with Connection Wires. This image shows a single QDY30A liquid level sensor, emphasizing the durable black cable and the color-coded wires (red and blue) at the end, ready for electrical connection. The sensor's body is made of stainless steel.




The sensor is available with various output options, including 4-20mA current output, 0-10V voltage output, or RS485 for digital communication, providing flexibility for integration into various control systems. The cable length varies by model, commonly ranging from 5m to longer custom lengths.

Figure 3: QDY30A Sensor with Coiled Cable. This image presents the QDY30A liquid level sensor with its extensive black cable coiled, illustrating the typical length provided for installation. The sensor's robust design and product label are clearly visible.




## 4. Specifications

| Parameter                | Details                                                      |
| ------------------------ | ------------------------------------------------------------ |
| Model                    | QDY30A (variants include QDY30A-B, etc.)                     |
| Type                     | Submersible Liquid Level Transmitter                         |
| Measured Medium          | Water, oil, fuels, or other non-corrosive liquids (corrosion-resistant options available) |
| Measuring Range          | 0-1m to 0-500m (customizable; common: 0-5m, 0-10m, etc.)     |
| Accuracy                 | 0.2% FS                                                      |
| Output Signals           | 4-20mA, 0-10V, 0-5V, 1-5V, RS485 (Modbus optional)           |
| Power Supply             | 12-36VDC (default 24VDC)                                     |
| Ambient Temperature      | -30°C to 80°C                                                |
| Temperature Compensation | -10°C to 70°C                                                |
| Overload Capacity        | <200% FS                                                     |
| Protection Level         | IP68                                                         |
| Long-term Stability      | ±0.2% FS/year                                                |
| Material                 | 304 Stainless Steel (probe); optional 316SS or PTFE for corrosive media |
| Probe Diameter           | Approximately 22-28mm                                        |
| Cable Length             | Customizable, typically 5m longer than range                 |
| Certification            | CE (where applicable)                                        |

(Note: Specifications may vary slightly by variant; refer to specific model for exact details. The above is compiled from standard data sheets and manuals.)

## 5. Setup

### 5.1 Unpacking and Inspection

Carefully remove the sensor from its packaging. Inspect the sensor body, cable, and connections for any signs of physical damage. Do not proceed with installation if any damage is observed. Contact your supplier for assistance.

### 5.2 Wiring

The QDY30A sensor typically uses a two-wire or three-wire connection depending on the output type. Ensure the power supply is disconnected before wiring.

- Red Wire: Positive power supply (V+).
- Black/Blue Wire: Ground/common (GND).
- For 4-20mA: Two-wire (red: signal + power, black: GND).
- For 0-10V: Three-wire (red: power, blue: GND, yellow/green: signal).
- For RS485: Additional wires for A/B data lines.

Always refer to the specific wiring diagram for your variant.

### 5.3 Installation Environment and Mounting

The sensor is designed for submersion. Ensure the installation point allows the sensor to be fully submerged to the lowest expected liquid level for accurate readings. The cable should be secured to prevent strain on the sensor connection point.

- Submersion: Lower the sensor into the liquid, ensuring the sensing head is below the minimum liquid level.
- Cable Management: Secure the cable to avoid movement and damage.
- Ventilation: The cable includes a vent tube for atmospheric pressure reference; keep the end dry and open.

## 6. Operating Instructions

### 6.1 Powering On

Apply the specified DC power supply. The sensor will begin measuring immediately.

### 6.2 Reading Output

The output is linear to the liquid level.

- 4-20mA: 4mA = zero level, 20mA = full scale.
- 0-10V: 0V = zero, 10V = full scale.
- RS485: Digital readout via Modbus protocol.

### 6.3 Calibration

Factory-calibrated; recalibration rarely needed.

## 7. Maintenance

- Clean the probe periodically to remove buildup.
- Inspect cable for damage.
- Store in cool, dry place if not in use.

## 8. Troubleshooting

| Problem               | Possible Cause                                  | Solution                                           |
| --------------------- | ----------------------------------------------- | -------------------------------------------------- |
| No Output             | Power issues, wiring errors, damage             | Check power, wiring, inspect for damage            |
| Inaccurate Readings   | Dirt on probe, blocked vent, improper scaling   | Clean probe, clear vent, verify settings           |
| Intermittent Readings | Loose connections, cable damage, unstable power | Secure connections, inspect cable, stabilize power |

## 9. Warranty and Support

Contact the manufacturer or supplier for warranty details. Typically 1-year warranty against defects.

(Note: This English specification sheet is based on translated and compiled information from available user manuals and product descriptions, as direct access to the original Chinese PDF was unavailable. It covers the key aspects of the QDY30A series.)