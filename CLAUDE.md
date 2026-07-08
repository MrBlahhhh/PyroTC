# CLAUDE.md — PyroTC

Guidance for Claude Code working in this repo. Read before editing or building.

## What this project is

`PyroTC` is the firmware for a **touchscreen BLE tire pyrometer**: a Waveshare ESP32-S3-Touch-LCD-1.28 (round GC9A01 LCD + CST816S touch) reading a MAX6675 K-type thermocouple, with a buzzer and battery indicator. The device owns all 12 tire readings (4 corners × OUT/MID/IN) and streams them over BLE to the companion Android app. It is the BLE **peripheral**; the app is the client.

Main source file: `src/main.cpp`. Keep the app logic single-file. The **exception** is the vendored LF-wake driver (`lf_wake.h/.cpp`, `cont_wake_data.h`) — see the TPMS section. The header comment at the top of `main.cpp` is authoritative for pinout and the BLE contract — trust it.

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

## TPMS: 125 kHz LF wake (implemented)

The device wakes Continental/Tesla TPMS sensors by replaying a captured **125 kHz OOK
wake telegram** over an external coil driver (IRLZ44N low-side switch + EL-50448 coil +
10 nF tank cap, on a separate 9 V domain — only GND shared with the ESP32).

- **Driver is vendored** from the `TpmsProbe-Tesla` project: `src/lf_wake.{h,cpp}` +
  the auto-generated `src/cont_wake_data.h` (184 uniform 128 µs half-bit slots, from an
  Autel scope capture). Do **not** hand-edit `cont_wake_data.h`; regenerate it upstream
  (`scripts/decode_scope_to_replay.py`) and re-copy all three files to re-sync.
- **Coil pin: `LF_COIL_PIN = 4`** (set in `platformio.ini build_flags`). GPIO4 → 100 Ω →
  MOSFET gate. Uses LEDC PWM; no clash with the digitalWrite buzzer.
- **UI:** a **WAKE** button (cyan) sits centre of the RECORD screen's bottom rim, between
  CLEAR (left) and BACK (right). Tapping it fires a **10 s burst** (`startWakeBurst`).
- **Timing is exclusive:** while `wakeActive`, `loop()` runs *only* `serviceLfWake()` and
  returns early — no MAX6675 read, LCD, touch, or BLE — because the ~220 ms thermocouple
  read and LCD draws would jitter the 128 µs LF bit grid. The gap loop calls `delay(1)` so
  the task watchdog stays fed. There is no mid-burst cancel (it auto-ends in 10 s).
- The old PRESS/SENS/MAP characteristics `…0004/0005/0006` are free; the wake telegram is
  one-way (no BLE), so no GATT change is needed. If TPMS *data* ever flows back through the
  device, coordinate new characteristics with the Android app (`PyroSync.kt`) in lockstep.

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
- This repo is **standalone** except for the vendored LF-wake driver (see the TPMS section). The Android app and other sibling projects are separate — coordinate the BLE contract with them, and don't pull other sibling code in here without an explicit, documented reason like the LF driver.
