# ESP8266-Pressure-gauge
# IVECO P/V — ESP8266 Pressure/Voltmeter with Web UI

A compact **ESP8266 (NodeMCU v3)** project that turns your phone into a live **voltmeter (0–50 V)** or **pressure gauge** (0–5/50/500/1000 bar) over Wi-Fi.  
It renders a responsive web dashboard with a top **LIVE scale**, a **digital display**, a **chart recorder** with **pause/play**, and a **movable vertical cursor** that shows the exact value at intersection.

> Designed for automotive linear 0–5 V pressure sensors and a switched divider for 0–50 V DC voltage probing.

---

## Features

- **Two modes** (hardware switch on D6):  
  - **Voltmeter** (0–50 V), safe-by-default at power-up  
  - **Pressure** (0–5, 0–50, 0–500, 0–1000 bar) with range buttons
- **Top LIVE scale** with a **red indicator bar**; labels adapt to range (e.g., `0.1K … 1.0K` for 1000 bar)
- **Large digital display** (tap to **Play/Pause** chart)
- **Chart recorder** (left-to-right sweep, 60 s window)  
  **Pause** freezes the frame; **vertical red marker** shows a **bold black point** at intersection with the curve and the exact value
- **Browser-side ring buffer** (~5 min) for quick review without stressing the MCU
- **Robust time mapping** (same time span for curve, marker, and hit test → no drift)
- **Mode & relay indicators** on GPIOs:
  - **D1 (GPIO5)** = HIGH in **Voltmeter**
  - **D2 (GPIO4)** = HIGH in **Pressure** (drives relay via transistor; also LED)
- **Fail-safe startup**: Voltmeter (0–50 V), relay OFF (high-range divider engaged)

---

## Hardware

- **Board**: NodeMCU v3 (ESP8266)
- **Analog**: A0 (use a 1 kΩ series + 100 nF to GND as RC protection)
- **Mode switch**: D6 with `INPUT_PULLUP` (HIGH=Voltmeter, LOW=Pressure)
- **Indicators / control**:
  - **D1 (GPIO5)** → LED “V” (with ~1 kΩ to 3.3 V)
  - **D2 (GPIO4)** → LED “P” **and** relay driver (NPN or logic-level NMOS). D2=HIGH in Pressure → **relay ON** (shorts R_add)

### Voltage divider (fail-safe)

- **Base divider (0–5 V):** `R_top_base ≈ 33 kΩ`, `R_bot = 56 kΩ`  
- **High-range add-on (0–50 V):** `R_add ≈ 787 kΩ` in series with `R_top_base`  
  - **Pressure mode:** relay **shorts** `R_add` → 0–5 V path  
  - **Voltmeter mode:** relay **OFF** → `R_add` in circuit → 0–50 V path  
- **Relay driver:** low-side NPN (2N2222/BC337) with base 2.2–4.7 kΩ, base-GND 100 kΩ, and a flyback diode across the coil (1N4148/1N5819). Coil: 5 V.

> This keeps the high-range divider **always present**; the relay only **shorts** it in Pressure mode.

---

## Pinout

| Function               | Pin         | Direction | Active |
|------------------------|-------------|-----------|--------|
| Mode switch P/V        | **D6**      | Input     | HIGH = Voltmeter |
| Voltmeter LED          | **D1 (GPIO5)** | Output | HIGH in Voltmeter |
| Pressure LED + Relay   | **D2 (GPIO4)** | Output | HIGH in Pressure |
| Analog input           | **A0**      | Input     | 0–3.2 V (ESP8266) |

---

## Firmware & Web UI

- **AP mode** (no password): `FRAME_AP`  
  Open: `http://192.168.4.1/`
- **Top scale**:  
  - Voltmeter: 0–50 V, major tick 1 V (labels every 5 V), minor 0.5 V  
  - Pressure: depends on range (e.g., 0–1000 bar shows `0.1K … 1.0K`)
- **Digital display**: tap to toggle **▶ / ⏸** (icon on the left)
- **Chart**: red curve; Y axis autoscaled to current range; vertical marker drag = exact value at intersection
- **Debounce**: non-blocking (≈20 ms) on D6; MCU loop remains responsive

---

## Build (Arduino IDE)

- **Board**: *NodeMCU 1.0 (ESP-12E)* (ESP8266 core 3.x)
- **Flash size**: 4M (1M SPIFFS) or no FS
- **CPU**: 80 MHz
- **Upload**: 921600 (or 460800)
- **Erase Flash**: *All Flash Contents* on first flash

Upload `src/iveco_pv.ino`, connect to `FRAME_AP`, open `http://192.168.4.1/`.

---

## Project Structure

