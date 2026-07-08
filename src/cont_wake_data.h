// Auto-generated from scope_csv_30_0.csv (verified vs scope_csv_29).
// UNIFORM 128 us half-bit slots (1 = 125 kHz carrier ON). Symmetric
// Manchester - NOT the skewed on/off segments (those broke decode).
#pragma once
#include <stdint.h>

#define CONT_WAKE_HALFBIT_US 128
#define CONT_WAKE_SLOT_COUNT 184
#define CONT_WAKE_DURATION_US 23552
#define CONT_WAKE_REPEAT_GAP_MS 5
#define CONT_WAKE_SYNC_INDEX 70

// 1 = carrier ON for 128 us, 0 = OFF for 128 us
static const uint8_t kContWakeSlots[] = {
  0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
  0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,1,1,1,0,0,0,1,0,1,1,
  0,0,1,1,0,0,1,0,1,0,0,1,0,1,1,0,1,0,1,0,1,0,0,1,1,0,0,1,1,0,0,1,0,1,0,1,0,1,1,0,
  1,0,1,0,1,0,0,1,1,0,1,0,0,1,0,1,0,1,0,1,1,0,1,0,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,
  0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1,0,1,1,0,1,0,0,1,
};
