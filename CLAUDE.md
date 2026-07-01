# CLAUDE.md — PyroTC

Guidance for Claude Code working in this repo. Read before editing or building.

## What this project is

`PyroTC` is the firmware for a **touchscreen BLE tire pyrometer**: a Waveshare ESP32-S3-Touch-LCD-1.28 (round GC9A01 LCD + CST816S touch) reading a MAX6675 K-type thermocouple, with a buzzer and battery indicator. The device owns all 12 tire readings (4 corners × OUT/MID/IN) and streams them over BLE to the companion Android app. It is the BLE **peripheral**; the app is the client.

Single source file: `src/main.cpp` (~450 lines). Keep it single-file. The header comment at the top of `main.cpp` is authoritative for pinout and the BLE contract — trust it.

## Build, flash, monitor

There is **no ESP32 toolchain in this environment** — do not try to build here. Verify edits by reasoning and by checking `{}`/`()` balance. The user builds/flashes on their own machine.

PlatformIO (single env, already configured — don't change board/platform casually):
- env `esp32-s3-devkitc-1`, pioarduino platform fork, `flash_size = 16MB`, monitor 115200 with `esp32_exception_decoder`.
- Libs: `adafruit/MAX6675@^1.1.0`, `moononournation/GFX Library for Arduino@^1.6.2`, `h2zero/NimBLE-Arduino@^2.1.0`.

```
pio run -t upload
pio device monitor -b 115200
```

## The BLE contract (DO NOT change without updating the Android app)

The Android app (`Trackday Pyrometer Helper`, `PyroSync.kt`) depends on these **exact** UUIDs and payload layouts. Changing any of them silently breaks the app.

- Device name: **`PyroTC`**
- Service: `a1b20001-7a9c-4b1e-9d3a-2f6c8e5d4c30`
- **TEMP** `…0002` (READ, NOTIFY ~4 Hz): 5 bytes — `float32 LE degC` + `uint8 fault`. Live probe temperature.
- **STATE** `…0003` (READ, NOTIFY on change): 24 bytes — `12 × int16 LE deci-degC`, order **LF[O,M,I], RF[O,M,I], LR[O,M,I], RR[O,M,I]**. `-32768` = empty slot.

If you must change the STATE layout, update `PyroSync.kt`'s parser in the app in lockstep and call it out explicitly.

## TPMS: LF wake-up planned (in progress)

Tesla TPMS support is coming back. The plan: the device gets **125 kHz LF wake-up hardware** (external driver stage) to wake Continental/Tesla TPMS sensors with a **modulated data telegram** (pattern is being scoped/captured now — not yet final). Planned integration:

- A **WAKE button** on the SELECT screen (bottom rim arc, `fillArc` style like RECORD's CLEAR/BACK).
- Carrier/telegram via LEDC PWM on a free GPIO (16 or 21 pencilled in; avoid strapping pins 0/3/45/46). Non-blocking transmit, same state-machine style as the buzzer.
- The old PRESS/SENS/MAP characteristics `…0004/0005/0006` were removed in an earlier cleanup and those UUIDs are currently free; if TPMS data flows through the device again, coordinate any new characteristics with the Android app (`PyroSync.kt`) in lockstep.

## Hard constraints

- **NimBLE-Arduino 2.x API only.** Use the patterns already in the file (`NimBLEDevice::init`, `server->createService`, `createCharacteristic(uuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY)`, `chState->setValue(buf, 24); chState->notify();`). Don't revert to the old `BLEDevice` API.
- **MAX6675 timing:** a conversion takes ~220 ms; the loop polls at **250 ms** (`lastTempMs >= 250`). Do not read faster — it returns garbage. MAX6675 is read-only SW-SPI (SCK 15, SDO 17, CS 18); there is no MOSI/SDI.
- **Pins are fixed by the board** (from the header): LCD DC8/CS9/SCK10/MOSI11/RST14/BL2; touch+IMU I2C SDA6/SCL7, TP_RST13, CST816 addr `0x15`; battery ADC GPIO1; buzzer GPIO33. Don't reassign onboard pins.
- **Keep the display/touch stack** (Arduino_GFX for GC9A01, CST816S over I2C). Don't swap graphics/touch libraries.
- `ENABLE_BLE` is a compile switch (`0` = build with no BLE for isolation testing). Keep it working.
- After any edit, confirm `{}` and `()` are balanced before handing back.

## UI / behavior to preserve

- Two screens via `enum Mode { SELECT, RECORD }`:
  - **SELECT**: four corner hit-rects (LF/RF/LR/RR) → sets `selTire`, switches to RECORD.
  - **RECORD**: live probe temp, O/M/I slots, big RECORD button (captures current temp into the active slot), CLEAR, BACK.
- RECORD/CLEAR push a `notifyState()` and set `dirty` for repaint. Buzzer gives capture feedback (`beep(...)`).
- Battery indicator top-right; charge state is **inferred from pack voltage** (`CHG_V_THRESH 4.15f`) because the board doesn't route a charge-status line to the MCU. `PIN_CHRG -1` unless the user wires the charger STAT pad to a GPIO.

## Tunables (compile-time `#define`s, top of file)

- `UNITS_FAHRENHEIT` — display units.
- `TOUCH_SWAP_XY` / `TOUCH_FLIP_X` / `TOUCH_FLIP_Y` — touch orientation vs display rotation. If taps land in the wrong place, these three cover all 8 orientations; don't rewrite the touch mapping, just flip these.
- `BUZZER_ACTIVE` / `BUZZER_FREQ`, `PIN_CHRG`, `CHG_V_THRESH`.

## Style & scope

- Match the existing terse C-with-Arduino style; static functions, small helpers, comment blocks like the header.
- This repo is **standalone**. The Android app and other sibling projects are separate — coordinate the BLE contract with them, but never pull their code in here.
