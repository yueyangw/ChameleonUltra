#ifndef ULC_CUSTOM_POLLING_H
#define ULC_CUSTOM_POLLING_H

#include <stdbool.h>
#include <stdint.h>

#define ULC_POLLING_CONFIG_VERSION 1

#define ULC_POLLING_DEFAULT_IDLE_MS     500
#define ULC_POLLING_DEFAULT_ACTIVE_MS   2000
#define ULC_POLLING_DEFAULT_COOLDOWN_MS 200
#define ULC_POLLING_DEFAULT_JITTER_PCT  10

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
} __attribute__((packed)) polling_config_t;

typedef struct {
    uint8_t enabled;
    uint8_t running;
    uint8_t current_slot;
    uint8_t field_active;
    uint16_t remaining_ms;
    uint16_t cycle_count;
} __attribute__((packed)) polling_status_t;

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

// Called by the NFC/LF emulation when the field state changes (optional hook).
void polling_note_field_state(bool active);

#endif
