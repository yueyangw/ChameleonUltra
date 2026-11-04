#ifndef ULC_CUSTOM_POLLING_H
#define ULC_CUSTOM_POLLING_H

#include <stdbool.h>
#include <stdint.h>

#include "app_util.h"

/*
 * Slot polling: while a reader field is present the emulated slot is swapped according to what the
 * reader does with the card (see engine_step() in polling.c), not on a timer.
 */

#define ULC_POLLING_CONFIG_VERSION 7

// Older records share the first 14 bytes (enable, slot mask, idle/active dwell, cooldown, ...).
#define ULC_POLLING_CONFIG_V2_VERSION  2
#define ULC_POLLING_CONFIG_V2_SIZE     16
#define ULC_POLLING_CONFIG_LEGACY_COMMON 14
#define ULC_POLLING_CONFIG_FIRST_SAME_SIZE 3
#define ULC_POLLING_CONFIG_LAST_LEGACY 6

#define ULC_POLLING_DEFAULT_IDLE_MS     500
#define ULC_POLLING_DEFAULT_ACTIVE_MS   2000
#define ULC_POLLING_DEFAULT_COOLDOWN_MS 200
#define ULC_POLLING_DEFAULT_JITTER_PCT  10
#define ULC_POLLING_DEFAULT_REPRESENT   1
#define ULC_POLLING_DEFAULT_FAIL_COUNT  3
#define ULC_POLLING_DEFAULT_DONE_QUIET_MS 100
#define ULC_POLLING_DEFAULT_PATIENCE_MS 800
#define ULC_POLLING_DEFAULT_FAIL_POLL_QUIET_MS 150
#define ULC_POLLING_DEFAULT_STICKY_SESSIONS 3
#define ULC_POLLING_DEFAULT_LOCK_QUIET_100MS 10

// Reader poll period estimation: REQA/WUPA intervals outside this range are
// not poll cycles (back-to-back REQA+WUPA pairs, field gaps).
#define ULC_POLLING_POLL_MIN_MS         3
#define ULC_POLLING_POLL_MAX_MS         1000
#define ULC_POLLING_POLL_SAMPLES        8
// Samples needed before the estimate is trusted.
#define ULC_POLLING_POLL_MIN_SAMPLES    4

// A failed auth only ends the visit once the exchange that just failed is over.
#define ULC_POLLING_FAIL_SETTLE_MS      20
// A reader that polled this often without selecting does not care for the slot.
#define ULC_POLLING_IGNORED_POLLS       3
#define ULC_POLLING_IGNORED_MIN_MS      120
// Poll period assumed while it has not been measured yet.
#define ULC_POLLING_DEFAULT_PERIOD_MS   100
// A field that returns within this time after being lost counts as a reset of the same reader,
// after longer it is a new discovery session (a phone looks for tags in short bursts).
#define ULC_POLLING_NEW_SESSION_MS      500
// The slot a reader worked on is kept this long while its field is paused.
#define ULC_POLLING_STICKY_HOLD_MS      10000

// Values of polling_status_t.phase
#define ULC_POLLING_PHASE_IDLE          0 // no field
#define ULC_POLLING_PHASE_FIELD         1 // field present, no reader interaction
#define ULC_POLLING_PHASE_HOLD          2 // slot kept, the reader is working on it
#define ULC_POLLING_PHASE_COOLDOWN      3 // field lost, waiting out cooldown

// Polling stops (and the device may sleep) after this long without any field,
// or twice the idle dwell if that is longer.
#define ULC_POLLING_NO_FIELD_TIMEOUT_MS 3000

typedef struct {
    uint8_t version;
    uint8_t enabled;
    uint16_t slot_mask;       // bit n -> slot n (0..15), 0: all
    uint16_t idle_dwell_ms;   // slot rotation while no field is present
    uint16_t active_dwell_ms; // slot rotation while a field is present but the reader does not poll
    uint16_t cooldown_ms;     // pause after the field was lost
    uint8_t adaptive;         // jitter on the idle rotation
    uint8_t jitter_percent;
    uint8_t order;            // 0: sequential, 1: random
    uint8_t keep_awake;
    uint16_t done_quiet_ms;   // UID-only reader: silence after the last read before the slot is left
    uint16_t patience_ms;     // failed auth, then silence (phone handing the tag to an app): slot is kept this long
    uint8_t sticky_sessions;  // reader pauses its field (phone): slot is kept for this many sessions, 0: off
    uint16_t fail_poll_quiet_ms; // failed auth, reader only looks for other cards: slot is left after this
    uint8_t represent_count;  // extra reads of a UID-only reader per slot (reads = 1 + this)
    uint8_t auth_fail_count;  // failed MF1 auths without success that end a visit, 0: off
    uint8_t lock_quiet_100ms; // slot with a successful auth is released after this much silence (x100 ms)
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
    uint16_t bad_mask;        // slots skipped once after failed auths
    uint8_t represents_left;  // unused, kept for the wire layout
    uint16_t poll_period_ms;  // estimated reader poll period, 0: unknown
} __attribute__((packed)) polling_status_t;

STATIC_ASSERT(sizeof(polling_status_t) == 20);

// ---- Lab support (development only, never persisted) ----------------------
#define ULC_POLLING_LAB_ANY_FIELD   0x01 // poll on any RF field, also when USB powered
#define ULC_POLLING_LAB_LOG         0x02 // record events into the ring buffer
#define ULC_POLLING_LAB_HOLD_IDLE   0x04 // do not rotate slots while no field is present (repeatable trials)

enum {
    PLOG_FIELD_ON = 1,
    PLOG_FIELD_OFF,
    PLOG_POLL,        // arg: measured interval to the previous poll, ms
    PLOG_SELECT,
    PLOG_AUTH_OK,
    PLOG_AUTH_FAIL,
    PLOG_HALT,
    PLOG_SWITCH,      // slot: new slot, arg: old slot
    PLOG_GAP_BEGIN,   // unused
    PLOG_GAP_END,     // unused
    PLOG_START,
    PLOG_STOP,
    PLOG_DECIDE,      // arg: reason code of the engine (PDEC_*)
    PLOG_PROF,        // slot: step (0 sense_end, 1 save, 2 set_slot, 3 load, 4 sense_run, 5 apply_slot_change), arg: ticks
};

enum {
    PDEC_AUTH_FAIL = 1,   // reader failed MF1 auth on the slot
    PDEC_READS_DONE,      // reader read and HALTed the card enough times
    PDEC_IGNORED,         // reader polls but never selects the slot
    PDEC_LOCK_IDLE,       // authenticated slot, reader went quiet
    PDEC_STALLED,         // selected, then nothing more
    PDEC_RF_RESET,        // field reset during an unfinished transaction
    PDEC_UNHALT,          // HALTed card re-armed for a reader that polls with REQA
    PDEC_SESSIONS,        // several discovery sessions of the reader without an auth success
    PDEC_SILENT,          // field without any reader polling
};

typedef struct {
    uint32_t tick;    // app_timer ticks (16384 Hz, 24 bit wrap)
    uint8_t event;
    uint8_t slot;
    uint16_t arg;
} __attribute__((packed)) polling_log_entry_t;

STATIC_ASSERT(sizeof(polling_log_entry_t) == 8);

typedef struct {
    uint8_t flags;    // ULC_POLLING_LAB_*
    uint8_t engine;   // unused, kept for the wire layout
    uint8_t clear_log;
    uint8_t reserved;
} __attribute__((packed)) polling_lab_t;

void polling_lab_set(const polling_lab_t *lab);
void polling_lab_get(polling_lab_t *lab);
uint32_t polling_log_total(void);
// Copies up to max entries starting at the absolute index `from`; returns how many.
uint16_t polling_log_read(uint32_t from, uint16_t max, polling_log_entry_t *out);

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

// Called from the NFCT interrupt for every received HF frame.
void polling_note_hf_activity(void);

// Called from the NFCT interrupt for every received frame longer than a REQA/WUPA
// (anticollision, select, RATS, authentication, data): the reader is working with the card.
void polling_note_hf_interaction(void);

// Called from the NFCT interrupt for every REQA/WUPA, used to learn the reader's poll period.
void polling_note_hf_poll(void);

// True when slot polling is enabled: the NFC peripheral is reset whenever the field is lost,
// because rapid slot swapping can otherwise leave it answering with garbage.
bool polling_wants_nfct_reset(void);

// Called from the NFCT interrupt when the reader HALTs the emulated card.
void polling_note_hf_halt(void);

// Called from the NFCT interrupt with the result of a MIFARE Classic auth.
void polling_note_mf1_auth(bool ok);

#endif
