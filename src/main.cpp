/*
 * PyroTC — BLE tire pyrometer (touchscreen-driven)
 * Waveshare ESP32-S3-Touch-LCD-1.28 (GC9A01 + CST816S touch) + MAX6675 (K-type) + buzzer.
 *
 * Touch UI: SELECT screen (4 corner buttons) -> RECORD screen (live temp, O/M/I slots,
 * big RECORD button, CLEAR, BACK). The device owns all 12 readings and streams them to
 * the app over BLE; no hardwired trigger needed.
 *
 * Onboard (fixed): LCD DC8 CS9 SCK10 MOSI11 RST14 BL2 | touch+IMU I2C SDA6 SCL7,
 * TP_RST 13 | BAT ADC GPIO1
 * You wire: MAX6675 (SW SPI) SCK15 SDO17 CS18 (+3V3,GND); buzzer GPIO33.
 *
 * BLE GATT (NimBLE, name "PyroTC"):
 * Service a1b20001-7a9c-4b1e-9d3a-2f6c8e5d4c30
 * TEMP  …0002 (READ,NOTIFY ~4Hz): float32 LE degC + uint8 fault   (live probe temp)
 * STATE …0003 (READ,NOTIFY on change): 12 × int16 LE deci-degC, order
 * LF[O,M,I], RF[O,M,I], LR[O,M,I], RR[O,M,I]; -32768 = empty slot
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <max6675.h>
#include <NimBLEDevice.h>

// display (fixed)
#define LCD_DC 8
#define LCD_CS 9
#define LCD_SCK 10
#define LCD_MOSI 11
#define LCD_RST 14
#define LCD_BL 2
#define PIN_BAT 1
#define TP_RST 13
#define I2C_SDA 6
#define I2C_SCL 7
#define CST816_ADDR 0x15

// wired peripherals (MAX6675 is read-only, no SDI/MOSI needed)
#define PIN_TC_SCK 15
#define PIN_TC_SDO 17
#define PIN_TC_CS  18
#define PIN_BUZZER 33
#define BUZZER_ACTIVE 1
#define BUZZER_FREQ 2700

#define ENABLE_BLE 1   // set 0 to build with NO BLE (isolation test)

// battery indicator (top-right). The board charges the cell but does NOT route a
// charge-status line to the MCU, so charging is inferred from pack voltage. If you
// wire the charger's CHRG/STAT pad to a GPIO (open-drain, low = charging), set
// PIN_CHRG to that pin for exact detection.
#define PIN_CHRG -1
#define CHG_V_THRESH 4.15f

#define UNITS_FAHRENHEIT 1
// Touch orientation for display rotation 1 (90°). If taps land wrong, adjust these
// three knobs (they cover all 8 orientations). For rotation 3 instead, use
// SWAP_XY 1 / FLIP_X 1 / FLIP_Y 0.
#define TOUCH_SWAP_XY 1
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 1

#define SVC_UUID     "a1b20001-7a9c-4b1e-9d3a-2f6c8e5d4c30"
#define CH_TEMP_UUID "a1b20002-7a9c-4b1e-9d3a-2f6c8e5d4c30"
#define CH_STATE_UUID "a1b20003-7a9c-4b1e-9d3a-2f6c8e5d4c30"

#define C_BG    0x0000
#define C_TEXT  0xFFFF
#define C_AMBER 0xFD80
#define C_GREEN 0x3EB4
#define C_RED   0xFAC9
#define C_MUTED 0x7C32
#define C_PANEL 0x18E3

Arduino_DataBus* bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, GFX_NOT_DEFINED);
Arduino_GFX* gfx = new Arduino_GC9A01(bus, LCD_RST, 1, true);

// MAX6675 uses SCK, CS, MISO(SDO)
MAX6675 maxtc(PIN_TC_SCK, PIN_TC_CS, PIN_TC_SDO);

NimBLECharacteristic* chTemp = nullptr;
NimBLECharacteristic* chState = nullptr;
volatile bool bleConnected = false;
volatile bool needStateSync = false;

const char* TIRE_SHORT[4] = {"LF", "RF", "LR", "RR"};
const char* TIRE_LONG[4]  = {"LEFT FRONT", "RIGHT FRONT", "LEFT REAR", "RIGHT REAR"};
const char* SLOT[3] = {"O", "M", "I"};

float temps[4][3];        // degC, NAN = empty
int selTire = -1;
enum Mode { SELECT, RECORD };
Mode mode = SELECT;

float lastTempC = NAN;
uint8_t lastFault = 0;
bool tcOk = true; // Assumed true for MAX6675, connection verified via NAN outputs
float battV = 0;
int battPct = 0;
bool charging = false;
float emaV = 0;
int lastBattPct = -1;
bool lastCharging = false;

bool dirty = true;
int lastShownTemp = -9999;
unsigned long lastTempMs = 0, lastBatMs = 0;

// touch edge detect
bool touchWasDown = false;

// ---------- buzzer ----------
int beepsLeft = 0; bool beepIsOn = false;
unsigned long beepNextToggle = 0, beepOnMs = 0, beepOffMs = 0;
static void buzzerOn()  {
#if BUZZER_ACTIVE
  digitalWrite(PIN_BUZZER, HIGH);
#else
  tone(PIN_BUZZER, BUZZER_FREQ);
#endif
}
static void buzzerOff() {
#if BUZZER_ACTIVE
  digitalWrite(PIN_BUZZER, LOW);
#else
  noTone(PIN_BUZZER);
#endif
}
static void beep(int count, unsigned long onMs, unsigned long offMs) {
  if (count <= 0) return;
  beepsLeft = count; beepOnMs = onMs; beepOffMs = offMs;
  beepIsOn = true; buzzerOn(); beepNextToggle = millis() + onMs;
}
static void serviceBuzzer() {
  if (beepsLeft <= 0 && !beepIsOn) return;
  unsigned long now = millis();
  if (now < beepNextToggle) return;
  if (beepIsOn) {
    buzzerOff(); beepIsOn = false; beepsLeft--;
    if (beepsLeft > 0) beepNextToggle = now + beepOffMs;
  } else if (beepsLeft > 0) {
    buzzerOn(); beepIsOn = true; beepNextToggle = now + beepOnMs;
  }
}

class ServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override { 
    bleConnected = true; 
    needStateSync = true; 
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override { 
    bleConnected = false; 
    NimBLEDevice::startAdvertising(); 
  }
};

static void sendTemp(float tC, uint8_t fault) {
#if ENABLE_BLE
  uint8_t buf[5]; memcpy(buf, &tC, 4); buf[4] = fault;
  chTemp->setValue(buf, 5); if (bleConnected) chTemp->notify();
#endif
}
static void notifyState() {
#if ENABLE_BLE
  uint8_t buf[24];
  for (int t = 0; t < 4; t++) for (int s = 0; s < 3; s++) {
    int idx = (t * 3 + s) * 2;
    float c = temps[t][s];
    int16_t v = isnan(c) ? (int16_t)-32768 : (int16_t)lroundf(c * 10.0f);
    buf[idx] = v & 0xFF; buf[idx + 1] = (v >> 8) & 0xFF;
  }
  chState->setValue(buf, 24); if (bleConnected) chState->notify();
#endif
}

// ---------- touch (CST816S) ----------
static bool readTouch(int& x, int& y) {
  uint8_t buf[6];
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(CST816_ADDR, 6) != 6) return false;
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();
  if ((buf[1] & 0x0F) == 0) return false;
  int rx = ((buf[2] & 0x0F) << 8) | buf[3];
  int ry = ((buf[4] & 0x0F) << 8) | buf[5];
#if TOUCH_SWAP_XY
  { int t = rx; rx = ry; ry = t; }
#endif
#if TOUCH_FLIP_X
  rx = 239 - rx;
#endif
#if TOUCH_FLIP_Y
  ry = 239 - ry;
#endif
  x = rx; y = ry; return true;
}
static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static float toShow(float c) { return UNITS_FAHRENHEIT ? (c * 9.0f / 5.0f + 32.0f) : c; }
static int filledCount(int t) { int n = 0; for (int s = 0; s < 3; s++) if (!isnan(temps[t][s])) n++; return n; }

// ---------- actions ----------
static void doRecord() {
  if (selTire < 0) return;
  if (!tcOk || lastFault != 0 || isnan(lastTempC)) { beep(2, 50, 70); return; }
  for (int s = 0; s < 3; s++) {
    if (isnan(temps[selTire][s])) {
      temps[selTire][s] = lastTempC;
      beep(1, 90, 0);
      notifyState(); dirty = true;
      return;
    }
  }
  beep(2, 50, 70);   // tire already full
}
static void doClear() {
  if (selTire < 0) return;
  for (int s = 0; s < 3; s++) temps[selTire][s] = NAN;
  beep(1, 90, 0); notifyState(); dirty = true;
}

// ---------- UI ----------
static void centerText(const char* s, int y, uint8_t size, uint16_t color) {
  int w = (int)strlen(s) * 6 * size;
  gfx->setTextColor(color); gfx->setTextSize(size);
  gfx->setCursor((240 - w) / 2, y); gfx->print(s);
}
static void textIn(const char* s, int cx, int cy, uint8_t size, uint16_t color) {
  int w = (int)strlen(s) * 6 * size, h = 8 * size;
  gfx->setTextColor(color); gfx->setTextSize(size);
  gfx->setCursor(cx - w / 2, cy - h / 2); gfx->print(s);
}

// ---------- battery ----------
static int batteryPercent(float v) {
  static const float vs[] = {3.30f,3.50f,3.60f,3.70f,3.75f,3.80f,3.85f,3.90f,3.95f,4.00f,4.10f,4.20f};
  static const int   ps[] = {0,    5,    10,   20,   30,   40,   50,   60,   70,   80,   92,   100};
  if (v <= vs[0]) return 0;
  if (v >= vs[11]) return 100;
  for (int i = 0; i < 11; i++) {
    if (v < vs[i + 1]) {
      float f = (v - vs[i]) / (vs[i + 1] - vs[i]);
      return (int)lroundf(ps[i] + f * (ps[i + 1] - ps[i]));
    }
  }
  return 100;
}

static bool chargingDetect(float v) {
#if (PIN_CHRG >= 0)
  (void)v; return digitalRead(PIN_CHRG) == LOW;     // open-drain CHRG: low = charging
#else
  static bool ch = false;                            // voltage heuristic + hysteresis
  if (v >= CHG_V_THRESH) ch = true;
  else if (v < CHG_V_THRESH - 0.10f) ch = false;
  return ch;
#endif
}

// Android-style battery glyph at top (right-biased): icon + %, green w/ bolt when charging.
static void drawBattery() {
  uint16_t col = charging ? C_GREEN
               : (battPct <= 15 ? C_RED : (battPct <= 40 ? C_AMBER : C_TEXT));
  gfx->fillRect(100, 10, 60, 18, C_BG);              // clear cluster region
  char p[6]; snprintf(p, sizeof(p), "%d%%", battPct);
  int w = (int)strlen(p) * 6;
  gfx->setTextSize(1); gfx->setTextColor(col);
  gfx->setCursor(130 - w, 14); gfx->print(p);        // % right-aligned to x=130
  gfx->drawRect(134, 12, 22, 12, col);               // body
  gfx->fillRect(156, 15, 2, 6, col);                 // terminal nub
  int fw = (int)(battPct / 100.0f * 18);
  if (fw > 0) gfx->fillRect(136, 14, fw, 8, col);    // fill
  if (charging) {                                    // lightning bolt
    gfx->fillTriangle(146, 11, 141, 19, 147, 19, C_TEXT);
    gfx->fillTriangle(145, 25, 150, 17, 144, 17, C_TEXT);
  }
}

// RECORD-screen slot geometry
static const int SLOT_Y = 54, SLOT_H = 64, SLOT_W = 58;
static const int SLOT_X[3] = {25, 91, 157};

static void drawSlot(int s, int nextEmpty) {
  int bx = SLOT_X[s], by = SLOT_Y, bw = SLOT_W, bh = SLOT_H;
  bool filled = !isnan(temps[selTire][s]);
  bool isNext = (s == nextEmpty);
  uint16_t border = filled ? C_GREEN : (isNext ? C_RED : C_MUTED);
  gfx->fillRect(bx + 2, by + 2, bw - 4, bh - 4, C_BG); 
  gfx->drawRoundRect(bx, by, bw, bh, 8, border);
  textIn(SLOT[s], bx + bw / 2, by + 14, 1, C_MUTED);
  char v[6]; uint16_t vc;
  if (filled) {
    snprintf(v, sizeof(v), "%d", (int)lroundf(toShow(temps[selTire][s]))); vc = C_GREEN;
  } else if (isNext) {
    vc = C_RED;
    if (tcOk && lastFault == 0 && !isnan(lastTempC))
      snprintf(v, sizeof(v), "%d", (int)lroundf(toShow(lastTempC)));
    else snprintf(v, sizeof(v), "--");
  } else { snprintf(v, sizeof(v), "--"); vc = C_MUTED; }
  textIn(v, bx + bw / 2, by + 42, 3, vc);
}

static void drawSelect() {
  gfx->fillScreen(C_BG);
  gfx->drawCircle(120, 120, 116, C_MUTED);
  drawBattery();

  const int bx[4] = {40, 128, 40, 128};
  const int by[4] = {40, 40, 128, 128};
  for (int t = 0; t < 4; t++) {
    int n = filledCount(t);
    uint16_t border = (n == 3) ? C_GREEN : (n > 0 ? C_AMBER : C_MUTED);
    gfx->fillRoundRect(bx[t], by[t], 72, 72, 10, C_PANEL);
    gfx->drawRoundRect(bx[t], by[t], 72, 72, 10, border);
    textIn(TIRE_SHORT[t], bx[t] + 36, by[t] + 27, 3, C_TEXT);
    char c[6]; snprintf(c, sizeof(c), "%d/3", n);
    textIn(c, bx[t] + 36, by[t] + 52, 2, border);
  }
}

static void drawRecord() {
  gfx->fillScreen(C_BG);
  gfx->drawCircle(120, 120, 116, C_MUTED);
  drawBattery();
  centerText(TIRE_LONG[selTire], 32, 2, C_AMBER);

  int nextEmpty = -1;
  for (int s = 0; s < 3; s++) if (isnan(temps[selTire][s])) { nextEmpty = s; break; }
  for (int s = 0; s < 3; s++) drawSlot(s, nextEmpty);

  gfx->fillRoundRect(26, 124, 188, 56, 12, C_AMBER);
  textIn("RECORD", 120, 152, 4, 0x1A03);
  
  gfx->drawRoundRect(46, 186, 68, 22, 6, C_RED);
  textIn("CLEAR", 80, 197, 2, C_RED);
  gfx->drawRoundRect(126, 186, 68, 22, 6, C_MUTED);
  textIn("BACK", 160, 197, 2, C_MUTED);
}

static void redraw() { if (mode == SELECT) drawSelect(); else drawRecord(); }

static void updateTempRegion() {
  if (mode == SELECT) {
    // nothing live to repaint on SELECT (battery handled separately)
  } else {
    int nextEmpty = -1;
    for (int s = 0; s < 3; s++) if (isnan(temps[selTire][s])) { nextEmpty = s; break; }
    if (nextEmpty >= 0) drawSlot(nextEmpty, nextEmpty);
  }
}

static void handleTap(int x, int y) {
  if (mode == SELECT) {
    if (inRect(x, y, 40, 40, 72, 72)) { selTire = 0; mode = RECORD; dirty = true; }
    else if (inRect(x, y, 128, 40, 72, 72)) { selTire = 1; mode = RECORD; dirty = true; }
    else if (inRect(x, y, 40, 128, 72, 72)) { selTire = 2; mode = RECORD; dirty = true; }
    else if (inRect(x, y, 128, 128, 72, 72)) { selTire = 3; mode = RECORD; dirty = true; }
  } else {
    if (inRect(x, y, 26, 124, 188, 56)) doRecord();
    else if (inRect(x, y, 46, 186, 68, 22)) doClear();
    else if (inRect(x, y, 126, 186, 68, 22)) { mode = SELECT; dirty = true; }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT); buzzerOff();
#if (PIN_CHRG >= 0)
  pinMode(PIN_CHRG, INPUT_PULLUP);
#endif
  pinMode(LCD_BL, OUTPUT); digitalWrite(LCD_BL, HIGH);
  analogSetAttenuation(ADC_11db);

  for (int t = 0; t < 4; t++) for (int s = 0; s < 3; s++) temps[t][s] = NAN;

  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW); delay(10);
  digitalWrite(TP_RST, HIGH); delay(60);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  gfx->begin();
  gfx->fillScreen(C_BG);
  centerText("PyroTC", 105, 4, C_AMBER);

  battV = 3.3f / 4096.0f * 3.0f * analogRead(PIN_BAT);
  emaV = battV; battPct = batteryPercent(emaV); charging = chargingDetect(emaV);
  lastBattPct = battPct; lastCharging = charging;

#if ENABLE_BLE
  NimBLEDevice::init("PyroTC");
  NimBLEDevice::setPower(9);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCb());
  NimBLEService* svc = server->createService(SVC_UUID);
  chTemp = svc->createCharacteristic(CH_TEMP_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chState = svc->createCharacteristic(CH_STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("PyroTC ready, advertising.");
#else
  Serial.println("PyroTC ready, BLE DISABLED (isolation build).");
#endif
  dirty = true;
}

void loop() {
  unsigned long now = millis();

  // MAX6675 takes ~220ms for a conversion. Polling at 250ms prevents garbage reads.
  if (now - lastTempMs >= 250) {
    lastTempMs = now;
    lastTempC = maxtc.readCelsius();
    lastFault = isnan(lastTempC) ? 1 : 0;
    sendTemp(isnan(lastTempC) ? -1000.0f : lastTempC, lastFault);
  }
  
  if (now - lastBatMs >= 2000) {
    lastBatMs = now;
    battV = 3.3f / 4096.0f * 3.0f * analogRead(PIN_BAT);
    emaV = (emaV <= 0.1f) ? battV : (emaV * 0.7f + battV * 0.3f);
    battPct = batteryPercent(emaV);
    charging = chargingDetect(emaV);
    if (battPct != lastBattPct || charging != lastCharging) {
      drawBattery();
      lastBattPct = battPct; lastCharging = charging;
    }
  }
  if (needStateSync) { needStateSync = false; notifyState(); }

  int tx, ty;
  bool down = readTouch(tx, ty);
  if (down && !touchWasDown) handleTap(tx, ty);
  touchWasDown = down;

  serviceBuzzer();

  int shownTemp = (int)lroundf(toShow(lastTempC));
  if (dirty) {
    redraw();
    dirty = false; lastShownTemp = shownTemp;
  } else if (shownTemp != lastShownTemp) {
    updateTempRegion();
    lastShownTemp = shownTemp;
  }
  delay(5);
}