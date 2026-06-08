# PyroTC — touchscreen BLE tire pyrometer (Waveshare ESP32-S3-Touch-LCD-1.28)

Standalone handheld K-type pyrometer. You pick the tire and record readings on the
round touchscreen; the device owns all 12 readings and streams them to the Cross
Weight app over BLE so cells fill live. No hardwired trigger.

## How it works
- **SELECT screen:** four corner buttons (LF/RF/LR/RR), each showing how many of its
  three readings are filled (n/3). Tap one.
- **RECORD screen:** shows the live probe temp big, the tire's O/M/I slots, and:
  - **RECORD** — captures the live temp into the next empty slot (O→M→I), beeps.
  - **CLEAR** — wipes that tire's three readings.
  - **BACK** — returns to SELECT.
- Beeps: one short = captured; two quick = no valid reading, or tire already full.

## Bill of materials
- Waveshare **ESP32-S3-Touch-LCD-1.28** (GC9A01 + CST816S touch) — you have it
- Adafruit **MAX31856** (K-type, SPI) + your K-type probe
- Active buzzer (or bare piezo — set `BUZZER_ACTIVE 0`)
- 3.7V LiPo on the MX1.25 connector (optional)

## Wiring — only 5 wires + power (no button anymore)
Display, touch+IMU (I2C 6/7), TP_RST(13), backlight(2), battery(1), USB-UART(43/44)
are already on the board.

| MAX31856 | ESP32-S3 |
|----------|----------|
| SCK | GPIO15 |
| SDI | GPIO16 |
| SDO | GPIO17 |
| CS  | GPIO18 |
| VIN | 3V3 |
| GND | GND |
| T+/T− | K-type probe (yellow +, red −) |

Buzzer: GPIO33 → (+), (−) → GND. (GPIO21 is now free/unused.)

## Build / flash (PlatformIO)
Flashes over the CH343P USB-UART; plain Serial works.
```
pio run -t upload
pio device monitor      # 115200
```

## BLE protocol (device name `PyroTC`)
Service `a1b20001-7a9c-4b1e-9d3a-2f6c8e5d4c30`
- **TEMP** `…0002` READ+NOTIFY ~8 Hz — float32 LE °C + uint8 fault (live probe temp).
- **STATE** `…0003` READ+NOTIFY on every record/clear — **24 bytes = 12 × int16 LE
  deci-°C**, order LF[O,M,I], RF[O,M,I], LR[O,M,I], RR[O,M,I]; `-32768` = empty slot.

The app mirrors STATE into the active session (index = tire×3 + slot; °C → session
unit). Temps are Celsius; the screen shows °F by default (`UNITS_FAHRENHEIT`).

## Notes / first-power checks
- **Backlight** is GPIO2 — if the board runs (BLE advertises, serial prints) but the
  screen is dark, that pin's the suspect.
- **Touch axes:** if taps land on the wrong button, flip `TOUCH_FLIP_X` / `TOUCH_FLIP_Y`
  (and tell me if X/Y need swapping for your panel orientation).
- Verify with **nRF Connect**: TEMP tracks the probe; tapping RECORD changes STATE.
