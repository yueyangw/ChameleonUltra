#ifndef ULC_CUSTOM_POLLING_H
#define ULC_CUSTOM_POLLING_H

#include <stdbool.h>
#include <stdint.h>

#include "app_util.h"

#define ULC_POLLING_CONFIG_VERSION 3

// Legacy v2 layout: 16 bytes, its first 14 bytes are identical to v3.
#define ULC_POLLING_CONFIG_V2_VERSION  2
#define ULC_POLLING_CONFIG_V2_SIZE     16
#define ULC_POLLING_CONFIG_V2_COMMON   14

#define ULC_POLLING_DEFAULT_IDLE_MS     500
#define ULC_POLLING_DEFAULT_ACTIVE_MS   2000
#define ULC_POLLING_DEFAULT_COOLDOWN_MS 200
#define ULC_POLLING_DEFAULT_JITTER_PCT  10
#define ULC_POLLING_DEFAULT_TXN_HOLD_MS 1500
#define ULC_POLLING_DEFAULT_HIT_BONUS_MS 1000
#define ULC_POLLING_DEFAULT_MISS_SHRINK_PCT 25

// HF frames closer together than this belong to the same reader transaction.
#define ULC_POLLING_TXN_GAP_MS          300
// A field dwell never shrinks below this percentage of active_dwell_ms.
#define ULC_POLLING_MIN_SCALE_PCT       40

// Values of polling_status_t.phase
#define ULC_POLLING_PHASE_IDLE          0 // no field
#define ULC_POLLING_PHASE_FIELD         1 // field present, no reader interaction
#define ULC_POLLING_PHASE_HOLD          2 // switch deferred, transaction in progress
#define ULC_POLLING_PHASE_COOLDOWN      3 // field lost, waiting out cooldown

// Polling stops (and the device may sleep) after this long without any field,
// or twice the idle dwell if that is longer.
#define ULC_POLLING_NO_FIELD_TIMEOUT_MS 3000

typedef struct {
    uint8_t version;
    uint8_t enabled;
    uint16_t slot_mask;       // bit n -> slot n (0..15)
    uint16_t idle_dwell_ms;   // no external field
    uint16_t active_dwell_ms; // field detected
    uint16_t cooldown_ms;     // after field lost
    uint8_t adaptive;
    uint8_t jitter_percent;
    uint8_t order;            // 0: sequential, 1: random
    uint8_t keep_awake;
    uint16_t txn_hold_ms;     // max time a running HF transaction defers a switch, 0: off
    uint16_t hit_bonus_ms;    // one-off dwell extension after a HF select, 0: off
    uint8_t miss_shrink_pct;  // field dwell shrink per visit without a select, 0: off
    uint8_t reserved[5];      // pad to a whole number of FDS words
} __attribute__((packed)) polling_config_t;

// FDS stores whole 32-bit words; the record must round-trip with the same length.
STATIC_ASSERT(sizeof(polling_config_t) == 24);

typedef struct {
    uint8_t enabled;
    uint8_t running;
    uint8_t current_slot;
    uint8_t field_active;
    uint32_t remaining_ms;
    uint16_t cycle_count;
    uint8_t woken_by_field;
    uint8_t phase;            // ULC_POLLING_PHASE_*
    uint16_t hit_count;       // HF selects since power-up
    uint8_t last_hit_slot;    // 0xFF: none yet
} __attribute__((packed)) polling_status_t;

STATIC_ASSERT(sizeof(polling_status_t) == 15);

extern volatile bool g_polling_keep_awake;

void polling_init(bool field_wake);
bool polling_load_config(void);
bool polling_save_config(void);

const polling_config_t *polling_get_config(void);
bool polling_set_config(const polling_config_t *config);
void polling_get_status(polling_status_t *status);

// Called from the main loop. Cheap when polling is disabled.
void polling_tick(void);

// Called when the user/host changes the active slot explicitly.
void polling_note_manual_switch(void);

// Called by the NFC/LF emulation when the field state changes. May run in
// interrupt context: it only records an event, polling_tick() consumes it.
void polling_note_field_state(bool active);

// Called from the NFCT interrupt when the reader has selected the emulated
// HF card (anti-collision completed). Only records an event.
void polling_note_hf_selected(void);

// Called from the NFCT interrupt for every received HF frame. Only records
// a timestamp, used to keep a running transaction from being cut short.
void polling_note_hf_activity(void);

#endif
