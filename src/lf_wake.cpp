#include "lf_wake.h"

#include <Arduino.h>

#include "cont_wake_data.h"

static const uint32_t LF_CARRIER_HZ = 125000;
static const uint8_t LF_OOK_DUTY = 128;  // 50% square when carrier ON

static bool lfReady = false;

// ---------------------------------------------------------------------------
// Continental wake is replayed as uniform 128 us half-bit slots from
// cont_wake_data.h. Symmetric Manchester (1 = carrier ON, 0 = OFF, 128 us each),
// verified against two independent DSO2C10 captures. The earlier variable
// on/off SEGMENT replay put out a skewed ~2:1 duty that only the most tolerant
// sensor could decode - this slot form keeps every transition on the 128 us grid.
// ---------------------------------------------------------------------------

static void lfDetachPwm() {
#if LF_WAKE_ENABLE
  ledcWrite(LF_COIL_PIN, 0);
  ledcDetach(LF_COIL_PIN);
  lfReady = false;
#endif
}

void lfWakeInit() {
#if LF_WAKE_ENABLE
  if (!ledcAttach(LF_COIL_PIN, LF_CARRIER_HZ, 8)) {
    Serial.println("LF ERR: ledcAttach failed on GPIO - check LF_COIL_PIN");
  }
  lfWakeCarrierOff();
  lfReady = true;
#endif
}

void lfWakeCarrierOff() {
#if LF_WAKE_ENABLE
  ledcWrite(LF_COIL_PIN, 0);
#endif
}

static inline void lfCarrierOn() {
  ledcWrite(LF_COIL_PIN, LF_OOK_DUTY);
}

void lfWakeCarrierOn() {
#if LF_WAKE_ENABLE
  if (!lfReady) lfWakeInit();
  lfCarrierOn();
#endif
}

void lfWakeCarrierTest(uint32_t holdMs) {
#if LF_WAKE_ENABLE
  if (!lfReady) lfWakeInit();
  lfCarrierOn();
  delay(holdMs);
  lfWakeCarrierOff();
#else
  (void)holdMs;
#endif
}

// ---------------------------------------------------------------------------
// PRIMARY PATH (CORRECTED): emit the captured wake as UNIFORM 128 us half-bit
// slots (cont_wake_data.h). Fixes the asymmetric-duty bug where the ESP32 put
// out ~202 us on / 63 us off instead of the real ~128/128. Two causes fixed:
//   (1) skewed on/off SEGMENT data replaced by uniform 128 us SLOTS;
//   (2) ledcWrite() switching latency: we set the carrier once per RUN of equal
//       slots and busy-wait to an ABSOLUTE micros() deadline, so per-call
//       overhead never accumulates and every half-bit boundary stays on grid.
// ---------------------------------------------------------------------------
static void lfSendSlots(const uint8_t* slots, size_t n) {
#if !LF_WAKE_ENABLE
  (void)slots; (void)n;
  return;
#else
  if (!lfReady) lfWakeInit();
  const uint32_t HB = CONT_WAKE_HALFBIT_US;   // 128 us
  const uint32_t t0 = micros();               // absolute time anchor
  uint32_t slot = 0;
  size_t i = 0;
  while (i < n) {
    uint8_t val = slots[i];
    size_t j = i;
    while (j < n && slots[j] == val) j++;     // run of identical slots
    uint32_t runSlots = (uint32_t)(j - i);
    if (val) lfCarrierOn(); else lfWakeCarrierOff();
    uint32_t deadline = t0 + (slot + runSlots) * HB;
    while ((int32_t)(micros() - deadline) < 0) { /* spin to grid */ }
    slot += runSlots;
    i = j;
  }
  lfWakeCarrierOff();
#endif
}

void triggerContinentalWake() {
  lfSendSlots(kContWakeSlots, CONT_WAKE_SLOT_COUNT);
}

void triggerContinentalWakeLoop(uint32_t reps) {
  for (uint32_t i = 0; i < reps; i++) {
    triggerContinentalWake();
    delay(CONT_WAKE_REPEAT_GAP_MS);
  }
  lfWakeCarrierOff();
}

// ---------------------------------------------------------------------------
// TEST HELPERS
// ---------------------------------------------------------------------------

void lfWakeGateBlinkTest(uint32_t secs) {
#if LF_WAKE_ENABLE
  lfDetachPwm();
  pinMode(LF_COIL_PIN, OUTPUT);
  const uint32_t t0 = millis();
  bool hi = false;
  while ((millis() - t0) < secs * 1000UL) {
    hi = !hi;
    digitalWrite(LF_COIL_PIN, hi ? HIGH : LOW);
    delay(500);
  }
  digitalWrite(LF_COIL_PIN, LOW);
  lfWakeInit();
#else
  (void)secs;
#endif
}

void lfWakeSlowCarrierTest(uint32_t holdMs) {
#if LF_WAKE_ENABLE
  lfDetachPwm();
  pinMode(LF_COIL_PIN, OUTPUT);
  const uint32_t end = millis() + holdMs;
  while ((int32_t)(end - millis()) > 0) {
    digitalWrite(LF_COIL_PIN, HIGH);
    delayMicroseconds(500);
    digitalWrite(LF_COIL_PIN, LOW);
    delayMicroseconds(500);
  }
  digitalWrite(LF_COIL_PIN, LOW);
  lfWakeInit();
#else
  (void)holdMs;
#endif
}

// ---------------------------------------------------------------------------
// LEGACY ASK-ENVELOPE PATHS (kept for A/B compare)
// ---------------------------------------------------------------------------
static void lfAskSlice(uint32_t onUs, uint32_t offUs) {
  lfCarrierOn();
  delayMicroseconds(onUs);
  lfWakeCarrierOff();
  delayMicroseconds(offUs);
}

static void lfAskTrain(uint32_t trainMs) {
  if (!lfReady) lfWakeInit();
  const uint32_t end = micros() + trainMs * 1000UL;
  while ((int32_t)(end - micros()) > 0)
    lfAskSlice(LF_CONT_ON_US, LF_CONT_OFF_US);
  lfWakeCarrierOff();
}

void triggerContinentalStyleWakeup() {
#if !LF_WAKE_ENABLE
  return;
#else
  lfAskTrain(LF_CONT_BURST_MS);
#endif
}

void triggerTs501StyleWakeup() {
#if !LF_WAKE_ENABLE
  return;
#else
  lfAskTrain(LF_TS501_PREAMBLE_MS);
  delay(LF_TS501_PACKET_GAP_MS);
  lfAskTrain(LF_TS501_MAIN_MS);
  lfWakeCarrierOff();
#endif
}

// ---------------------------------------------------------------------------
// TESLA MANCHESTER PATHS (kept; not Continental)
// ---------------------------------------------------------------------------
static void lfManchesterBit(bool one) {
  if (one) {
    lfWakeCarrierOff();
    delayMicroseconds(LF_HALF_BIT_US);
    lfCarrierOn();
    delayMicroseconds(LF_HALF_BIT_US);
  } else {
    lfCarrierOn();
    delayMicroseconds(LF_HALF_BIT_US);
    lfWakeCarrierOff();
    delayMicroseconds(LF_HALF_BIT_US);
  }
}

static void sendRawByte(uint8_t b) {
  for (int i = 7; i >= 0; i--)
    lfManchesterBit((b >> i) & 1);
}

static void lfSendFrame(const uint8_t* frame, size_t len) {
#if !LF_WAKE_ENABLE
  (void)frame;
  (void)len;
  return;
#else
  if (!lfReady) lfWakeInit();
  for (size_t i = 0; i < len; i++)
    sendRawByte(frame[i]);
  lfWakeCarrierOff();
#endif
}

void triggerStandardTeslaWakeup() {
  static const uint8_t kFrame[] = {
      0xAA, 0xAA, 0x7E, 0x69, 0xB1, 0x90, 0x07, 0x01,
  };
  lfSendFrame(kFrame, sizeof(kFrame));
}

void triggerCybertruckWakeup() {
  static const uint8_t kFrame[] = {
      0xAA, 0xAA, 0x7E, 0xB7, 0x8D, 0x37, 0x39, 0x01,
  };
  lfSendFrame(kFrame, sizeof(kFrame));
}
