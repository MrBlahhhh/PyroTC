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
 *
 * TPMS: Tesla Model 3/Y (all gens) + Model S/X (2021+) BLE sensors.
 * Sensors advertise as "tsTPMS" with manufacturer data containing 0x2B 0x02 payload.
 * Decode based on cunzulatu/Tesla_BLE_TPMS — see decodeTeslaTpms() below.
 * PRESS characteristic publishes 4xfloat PSI + 4xfloat degC (or NaN until heard).
 * Build once with SCAN_DUMP 1 and check Serial @115200 to discover your 4 MACs.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <max6675.h>
#include <NimBLEDevice.h>
#include <string>

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

#define PIN_CHRG -1
#define CHG_V_THRESH 4.15f

#define UNITS_FAHRENHEIT 1
#define TOUCH_SWAP_XY 1
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 1

#define SVC_UUID     "a1b20001-7a9c-4b1e-9d3a-2f6c8e5d4c30"
#define CH_TEMP_UUID "a1b20002-7a9c-4b1e-9d3a-2f6c8e5d4c30"
#define CH_STATE_UUID "a1b20003-7a9c-4b1e-9d3a-2f6c8e5d4c30"
#define CH_PRESS_UUID "a1b20004-7a9c-4b1e-9d3a-2f6c8e5d4c30"

// ---- TPMS (Tesla BLE sensors) ----
// Tesla Model 3/Y (all gens) + Model S/X (2021+) BLE sensors.
// Sensors advertise as "tsTPMS" with manufacturer data containing 0x2B 0x02
// payload marker. Build once with SCAN_DUMP 1 and watch Serial @115200 to
// discover your four sensor MAC addresses.
//
// Decode (from cunzulatu/Tesla_BLE_TPMS, MIT license):
//   Payload at 0x2B 0x02 marker:
//     [0x2B] [0x02] [?] [?] [type] [press_lo] [press_hi] [temp] [batt_lo] [batt_hi]
//     type < 0x05 -> sleep mode (no data)
//     Pressure: ((uint16 LE @ +5) - 100) / 7 = PSI
//     Temperature: (uint8 @ +7) - 1 = degF
#define SCAN_DUMP 0
#define TPMS_STALE_MS 90000UL
static const char* TPMS_MAC[4] = {
  "00:00:00:00:00:00",  // LF
  "00:00:00:00:00:00",  // RF
  "00:00:00:00:00:00",  // LR
  "00:00:00:00:00:00",  // RR
};

// ----- Thread-safe TPMS state -----
// The NimBLE scan callback runs in a different FreeRTOS task than loop().
// All access to these arrays is protected by tpmsLock.
static portMUX_TYPE tpmsLock = portMUX_INITIALIZER_UNLOCKED;
static float    tpmsPsi[4]     = {NAN, NAN, NAN, NAN};
static float    tpmsTempC[4]   = {NAN, NAN, NAN, NAN};
static uint32_t tpmsLastMs[4]  = {0, 0, 0, 0};
static bool     pressDirty      = false;

// BLE connection state -- also protected (NimBLE callbacks run in host task)
static portMUX_TYPE bleLock = portMUX_INITIALIZER_UNLOCKED;
static bool bleConnected = false;
static bool needStateSync = false;

#define C_BG    0x0000
#define C_TEXT  0xFFFF
#define C_AMBER 0xFD80
#define C_GREEN 0x3EB4
#define C_RED   0xFAC9
#define C_MUTED 0x7C32
#define C_PANEL 0x18E3
#define C_TPMSBG 0x02D6

Arduino_DataBus* bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, GFX_NOT_DEFINED);
Arduino_GFX* gfx = new Arduino_GC9A01(bus, LCD_RST, 1, true);
MAX6675 maxtc(PIN_TC_SCK, PIN_TC_CS, PIN_TC_SDO);

NimBLECharacteristic* chTemp = nullptr;
NimBLECharacteristic* chState = nullptr;
NimBLECharacteristic* chPress = nullptr;

const char* TIRE_SHORT[4] = {"LF", "RF", "LR", "RR"};
const char* TIRE_LONG[4]  = {"LEFT FRONT", "RIGHT FRONT", "LEFT REAR", "RIGHT REAR"};
const char* SLOT[3] = {"O", "M", "I"};

float temps[4][3];
int selTire = -1;
enum Mode { SELECT, RECORD };
Mode mode = SELECT;

float lastTempC = NAN;
uint8_t lastFault = 0;
bool tcOk = true;
float battV = 0;
int battPct = 0;
bool charging = false;
float emaV = 0;
int lastBattPct = -1;
bool lastCharging = false;

bool dirty = true;
int lastShownTemp = -9999;
unsigned long lastTempMs = 0, lastBatMs = 0;
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
    portENTER_CRITICAL(&bleLock);
    bleConnected = true;
    needStateSync = true;
    portEXIT_CRITICAL(&bleLock);
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    portENTER_CRITICAL(&bleLock);
    bleConnected = false;
    portEXIT_CRITICAL(&bleLock);
    NimBLEDevice::startAdvertising();
  }
};

static bool bleIsConnected() {
  portENTER_CRITICAL(&bleLock);
  bool c = bleConnected;
  portEXIT_CRITICAL(&bleLock);
  return c;
}

static bool bleConsumeStateSync() {
  portENTER_CRITICAL(&bleLock);
  bool s = needStateSync;
  needStateSync = false;
  portEXIT_CRITICAL(&bleLock);
  return s;
}

static void sendTemp(float tC, uint8_t fault) {
#if ENABLE_BLE
  uint8_t buf[5]; memcpy(buf, &tC, 4); buf[4] = fault;
  chTemp->setValue(buf, 5); if (bleIsConnected()) chTemp->notify();
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
  chState->setValue(buf, 24); if (bleIsConnected()) chState->notify();
#endif
}

// ---------- Tesla TPMS decoder ----------
// Payload structure after 0x2B 0x02 marker:
//   [0x2B] [0x02] [?] [?] [type] [press_lo] [press_hi] [temp] [batt_lo] [batt_hi]
//   type < 0x05 -> SLEEP MODE (no pressure/temp data)
//   pressure: ((uint16 LE) - 100) / 7 = PSI
//   temperature: uint8 - 1 = degF
// Ref: cunzulatu/Tesla_BLE_TPMS (MIT)
static int findTeslaPayload(const uint8_t* d, size_t n) {
  for (size_t i = 0; i + 10 <= n; i++) {
    if (d[i] == 0x2B && d[i + 1] == 0x02) return (int)i;
  }
  return -1;
}

static bool decodeTeslaTpms(const uint8_t* d, size_t n, float& psi, float& tempC) {
  int idx = findTeslaPayload(d, n);
  if (idx < 0 || idx + 10 > (int)n) return false;

  uint8_t type = d[idx + 4];
  if (type < 0x05) return false;  // sensor in sleep mode -- no data

  uint16_t rawP = (uint16_t)d[idx + 5] | ((uint16_t)d[idx + 6] << 8);
  uint8_t  rawT = d[idx + 7];

  psi   = ((float)rawP - 100.0f) / 7.0f;
  tempC = ((float)(rawT - 1) - 32.0f) * 5.0f / 9.0f;  // degF -> degC

  // Sanity gate -- track-range thresholds
  if (psi < 0 || psi > 150 || tempC < -40 || tempC > 200) return false;
  return true;
}

// Thread-safe write -- call ONLY from NimBLE scan callback (different task)
static void tpmsWrite(int w, float psi, float tempC) {
  portENTER_CRITICAL(&tpmsLock);
  tpmsPsi[w]    = psi;
  tpmsTempC[w]  = tempC;
  tpmsLastMs[w] = millis();
  pressDirty    = true;
  portEXIT_CRITICAL(&tpmsLock);
}

static inline bool hasTpms(int w) {
  portENTER_CRITICAL(&tpmsLock);
  bool ok = tpmsLastMs[w] != 0 && (millis() - tpmsLastMs[w]) < TPMS_STALE_MS;
  portEXIT_CRITICAL(&tpmsLock);
  return ok;
}

static int wheelForAddr(const std::string& addr) {
  for (int w = 0; w < 4; w++) if (addr == TPMS_MAC[w]) return w;
  return -1;
}

static bool tpmsConsumeDirty() {
  portENTER_CRITICAL(&tpmsLock);
  bool d = pressDirty;
  pressDirty = false;
  portEXIT_CRITICAL(&tpmsLock);
  return d;
}

static void notifyPressures() {
#if ENABLE_BLE
  uint8_t buf[32];
  portENTER_CRITICAL(&tpmsLock);
  for (int w = 0; w < 4; w++) { float p = tpmsPsi[w];   memcpy(buf + w * 4,      &p, 4); }
  for (int w = 0; w < 4; w++) { float t = tpmsTempC[w]; memcpy(buf + 16 + w * 4, &t, 4); }
  portEXIT_CRITICAL(&tpmsLock);
  chPress->setValue(buf, 32); if (bleIsConnected()) chPress->notify();
#endif
}

#if ENABLE_BLE
class ScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string addr = dev->getAddress().toString();
    std::string md = dev->getManufacturerData();
#if SCAN_DUMP
    Serial.printf("ADV %s rssi=%d name=%s mfg=", addr.c_str(), dev->getRSSI(), dev->getName().c_str());
    for (size_t i = 0; i < md.size(); i++) Serial.printf("%02x", (uint8_t)md[i]);
    int teslaIdx = findTeslaPayload((const uint8_t*)md.data(), md.size());
    if (teslaIdx >= 0) {
      uint8_t type = (uint8_t)md[teslaIdx + 4];
      Serial.printf("  [TESLA type=0x%02x %s]", type, type < 0x05 ? "SLEEP" : "ACTIVE");
    }
    Serial.println();
#endif
    int w = wheelForAddr(addr);
    if (w < 0) return;
    float psi, tc;
    if (decodeTeslaTpms((const uint8_t*)md.data(), md.size(), psi, tc)) {
      tpmsWrite(w, psi, tc);
    }
  }
};
#endif

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
  beep(2, 50, 70);
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

static int batteryPercent(float v) {
  static const float vs[] = {3.30f,3.50f,3.60f,3.70f,3.75f,3.80f,3.85f,3.90f,3.95f,4.00f,4.10f,4.20f};
  static const int   ps[] = {0,5,10,20,30,40,50,60,70,80,92,100};
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
  (void)v; return digitalRead(PIN_CHRG) == LOW;
#else
  static bool ch = false;
  if (v >= CHG_V_THRESH) ch = true;
  else if (v < CHG_V_THRESH - 0.10f) ch = false;
  return ch;
#endif
}

static void drawBattery() {
  uint16_t col = charging ? C_GREEN
               : (battPct <= 15 ? C_RED : (battPct <= 40 ? C_AMBER : C_TEXT));
  gfx->fillRect(100, 10, 60, 18, C_BG);
  char p[6]; snprintf(p, sizeof(p), "%d%%", battPct);
  int w = (int)strlen(p) * 6;
  gfx->setTextSize(1); gfx->setTextColor(col);
  gfx->setCursor(130 - w, 14); gfx->print(p);
  gfx->drawRect(134, 12, 22, 12, col);
  gfx->fillRect(156, 15, 2, 6, col);
  int fw = (int)(battPct / 100.0f * 18);
  if (fw > 0) gfx->fillRect(136, 14, fw, 8, col);
  if (charging) {
    gfx->fillTriangle(146, 11, 141, 19, 147, 19, C_TEXT);
    gfx->fillTriangle(145, 25, 150, 17, 144, 17, C_TEXT);
  }
}

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

    // thread-safe TPMS read
    float pPsi; uint32_t pLast;
    portENTER_CRITICAL(&tpmsLock);
    pPsi = tpmsPsi[t]; pLast = tpmsLastMs[t];
    portEXIT_CRITICAL(&tpmsLock);
    bool tp = pLast != 0 && (millis() - pLast) < TPMS_STALE_MS;

    gfx->fillRoundRect(bx[t], by[t], 72, 72, 10, tp ? C_TPMSBG : C_PANEL);
    gfx->drawRoundRect(bx[t], by[t], 72, 72, 10, border);
    if (tp && !isnan(pPsi)) {
      char p[8]; snprintf(p, sizeof(p), "%dp", (int)lroundf(pPsi));
      textIn(p, bx[t] + 36, by[t] + 11, 1, C_TEXT);
    }
    textIn(TIRE_SHORT[t], bx[t] + 36, by[t] + 30, 3, C_TEXT);
    char c[6]; snprintf(c, sizeof(c), "%d/3", n);
    textIn(c, bx[t] + 36, by[t] + 54, 2, border);
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
  if (mode != SELECT) {
    int nextEmpty = -1;
    for (int s = 0; s < 3; s++) if (isnan(temps[selTire][s])) { nextEmpty = s; break; }
    if (nextEmpty >= 0) drawSlot(nextEmpty, nextEmpty);
  }
}

static void handleTap(int x, int y) {
  if (mode == SELECT) {
    if (inRect(x, y, 40, 40, 72, 72))      { selTire = 0; mode = RECORD; dirty = true; }
    else if (inRect(x, y, 128, 40, 72, 72)) { selTire = 1; mode = RECORD; dirty = true; }
    else if (inRect(x, y, 40, 128, 72, 72)) { selTire = 2; mode = RECORD; dirty = true; }
    else if (inRect(x, y, 128, 128, 72, 72)){ selTire = 3; mode = RECORD; dirty = true; }
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
  chPress = svc->createCharacteristic(CH_PRESS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->enableScanResponse(true);
  adv->start();

  static ScanCb scanCb;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCb, false);
  scan->setActiveScan(true);
  scan->setInterval(160);
  scan->setWindow(160);
  scan->setDuplicateFilter(false);
  scan->start(0, false);
  notifyPressures();
  Serial.println("PyroTC ready, advertising + Tesla TPMS scan.");
#else
  Serial.println("PyroTC ready, BLE DISABLED (isolation build).");
#endif
  dirty = true;
}

void loop() {
  unsigned long now = millis();

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

  if (bleConsumeStateSync()) notifyState();
  if (tpmsConsumeDirty()) notifyPressures();

  static uint8_t lastTpmsMask = 0xFF;
  uint8_t tmask = 0;
  for (int w = 0; w < 4; w++) if (hasTpms(w)) tmask |= (1 << w);
  if (tmask != lastTpmsMask) {
    lastTpmsMask = tmask;
    if (mode == SELECT) dirty = true;
  }

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