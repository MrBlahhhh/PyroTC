#pragma once

#include <stdint.h>

// 125 kHz OOK coil driver (GPIO4 -> 100R -> IRLZ44N gate, 9 V -> coil+C14).

#ifndef LF_COIL_PIN
#define LF_COIL_PIN 4
#endif

#ifndef LF_WAKE_ENABLE
#define LF_WAKE_ENABLE 1
#endif

// Manchester / half-bit timing. 256 us bit = ~3.9 kbps (Continental LF).
#ifndef LF_MANCHESTER_BIT_US
#define LF_MANCHESTER_BIT_US 256
#endif
#define LF_HALF_BIT_US (LF_MANCHESTER_BIT_US / 2)   // 128 us

// Gap between repeated wake telegrams (from 30 ms capture: ~5 ms).
#ifndef LF_CONT_REPEAT_GAP_MS
#define LF_CONT_REPEAT_GAP_MS 5
#endif

// ---- legacy ASK-envelope params (kept for compare key 'p'/'f') ----
#ifndef LF_CONT_ON_US
#define LF_CONT_ON_US 170
#endif
#ifndef LF_CONT_OFF_US
#define LF_CONT_OFF_US 90
#endif
#ifndef LF_TS501_PREAMBLE_MS
#define LF_TS501_PREAMBLE_MS 2
#endif
#ifndef LF_TS501_PACKET_GAP_MS
#define LF_TS501_PACKET_GAP_MS 5
#endif
#ifndef LF_TS501_MAIN_MS
#define LF_TS501_MAIN_MS 25
#endif
#ifndef LF_TS501_REPEAT_MS
#define LF_TS501_REPEAT_MS 50
#endif
#ifndef LF_CONT_BURST_MS
#define LF_CONT_BURST_MS 30
#endif

void lfWakeInit();
void lfWakeCarrierOff();
void lfWakeCarrierOn();
void lfWakeCarrierTest(uint32_t holdMs);
void lfWakeGateBlinkTest(uint32_t secs);
void lfWakeSlowCarrierTest(uint32_t holdMs);

// Primary: raw half-bit replay of the captured Autel Continental wake.
void triggerContinentalWake();          // one telegram
void triggerContinentalWakeLoop(uint32_t reps);  // reps telegrams w/ gap

// legacy / other-protocol (kept)
void triggerTs501StyleWakeup();
void triggerContinentalStyleWakeup();
void triggerStandardTeslaWakeup();
void triggerCybertruckWakeup();
