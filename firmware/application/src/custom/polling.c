#include "polling.h"

#include <stdlib.h>
#include <string.h>

#include "app_timer.h"
#include "fds_ids.h"
#include "fds_util.h"
#include "rfid_main.h"
#include "syssleep.h"
#include "tag_emulation.h"

#define NRF_LOG_MODULE_NAME polling
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();

#define POLLING_MIN_DWELL_MS 50

volatile bool g_polling_keep_awake = false;

// FDS writes whole words straight from this buffer, keep it word aligned.
static polling_config_t s_config __attribute__((aligned(4)));
static uint32_t s_last_switch;
static uint32_t s_current_dwell_ms;   // dwell of the current slot, computed once per switch
static bool s_current_dwell_field;    // field state s_current_dwell_ms was computed for
static bool s_in_cooldown;
static uint32_t s_cooldown_start;
static uint16_t s_cycle_count;
static bool s_running;
static bool s_woken_by_field;         // set once at boot, fixed for this power cycle
static bool s_manual_stop;            // sticky until the host pushes a new config
static bool s_timed_out;              // no field seen for too long

// Per-visit state of the current slot, cleared on every switch
static bool s_visit_field_lost;       // field was absent at some point of this visit
static bool s_visit_hit;              // reader selected this slot during the visit
static bool s_visit_activity;         // HF frames were seen during the visit
static bool s_holding;                // switch is being deferred by a transaction
static uint32_t s_visit_bonus_ms;     // one-off hit extension of this visit's field dwell
static uint32_t s_txn_start;          // tick of the first HF frame of the visit

static uint8_t s_slot_scale[TAG_MAX_SLOT_NUM]; // field dwell scale per slot, in percent
static uint16_t s_hit_count;
static uint8_t s_last_hit_slot = 0xFF;

// Written from the NFCT / LPCOMP interrupt handlers, consumed by polling_tick().
static volatile bool s_evt_field_on;
static volatile bool s_evt_field_off;
static volatile uint32_t s_last_field_tick;
static volatile bool s_evt_hf_selected;
static volatile bool s_evt_hf_activity;
static volatile uint32_t s_last_activity_tick;

static void polling_set_defaults(void) {
    memset(&s_config, 0, sizeof(s_config));
    s_config.version = ULC_POLLING_CONFIG_VERSION;
    s_config.enabled = 0;
    s_config.slot_mask = 0;
    s_config.idle_dwell_ms = ULC_POLLING_DEFAULT_IDLE_MS;
    s_config.active_dwell_ms = ULC_POLLING_DEFAULT_ACTIVE_MS;
    s_config.cooldown_ms = ULC_POLLING_DEFAULT_COOLDOWN_MS;
    s_config.adaptive = 1;
    s_config.jitter_percent = ULC_POLLING_DEFAULT_JITTER_PCT;
    s_config.order = 0;
    s_config.keep_awake = 1;
    s_config.txn_hold_ms = ULC_POLLING_DEFAULT_TXN_HOLD_MS;
    s_config.hit_bonus_ms = ULC_POLLING_DEFAULT_HIT_BONUS_MS;
    s_config.miss_shrink_pct = ULC_POLLING_DEFAULT_MISS_SHRINK_PCT;
}

static bool polling_config_valid(const polling_config_t *cfg) {
    if (cfg == NULL || cfg->version != ULC_POLLING_CONFIG_VERSION) {
        return false;
    }
    if (cfg->enabled > 1 || cfg->adaptive > 1 || cfg->keep_awake > 1 || cfg->order > 1) {
        return false;
    }
    if (cfg->idle_dwell_ms < POLLING_MIN_DWELL_MS || cfg->idle_dwell_ms > 60000) {
        return false;
    }
    if (cfg->active_dwell_ms < POLLING_MIN_DWELL_MS || cfg->active_dwell_ms > 60000) {
        return false;
    }
    if (cfg->cooldown_ms > 10000) {
        return false;
    }
    if (cfg->jitter_percent > 50) {
        return false;
    }
    if (cfg->txn_hold_ms > 10000 || cfg->hit_bonus_ms > 10000 || cfg->miss_shrink_pct > 50) {
        return false;
    }
    return true;
}

static bool slot_usable(uint8_t slot) {
    if (slot >= TAG_MAX_SLOT_NUM) {
        return false;
    }
    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(slot, &types);
    return (is_slot_enabled(slot, TAG_SENSE_HF) && types.tag_hf != TAG_TYPE_UNDEFINED) ||
           (is_slot_enabled(slot, TAG_SENSE_LF) && types.tag_lf != TAG_TYPE_UNDEFINED);
}

static bool slot_selected(uint8_t slot) {
    uint16_t mask = s_config.slot_mask == 0 ? 0xFFFFU : s_config.slot_mask;
    return (mask & (1U << slot)) != 0 && slot_usable(slot);
}

static uint8_t next_selected_slot(uint8_t from) {
    if (s_config.order == 1) {
        // Random: pick uniformly among the other selected slots
        uint8_t candidates[TAG_MAX_SLOT_NUM];
        uint8_t count = 0;
        for (uint8_t slot = 0; slot < TAG_MAX_SLOT_NUM; slot++) {
            if (slot != from && slot_selected(slot)) {
                candidates[count++] = slot;
            }
        }
        return count == 0 ? from : candidates[rand() % count];
    }
    for (uint8_t i = 1; i <= TAG_MAX_SLOT_NUM; i++) {
        uint8_t slot = (uint8_t)((from + i) % TAG_MAX_SLOT_NUM);
        if (slot_selected(slot)) {
            return slot;
        }
    }
    return from;
}

static bool slot_has_hf(uint8_t slot) {
    tag_slot_specific_type_t types;
    tag_emulation_get_specific_types_by_slot(slot, &types);
    return is_slot_enabled(slot, TAG_SENSE_HF) && types.tag_hf != TAG_TYPE_UNDEFINED;
}

static void polling_reset_scales(void) {
    for (uint8_t i = 0; i < TAG_MAX_SLOT_NUM; i++) {
        s_slot_scale[i] = 100;
    }
}

static void polling_reset_visit(bool field) {
    s_visit_field_lost = !field;
    s_visit_hit = false;
    s_visit_activity = false;
    s_holding = false;
    s_visit_bonus_ms = 0;
}

static uint32_t effective_dwell_ms(bool field_active, uint8_t slot) {
    uint32_t base = field_active ? s_config.active_dwell_ms : s_config.idle_dwell_ms;
    if (field_active) {
        base = base * s_slot_scale[slot] / 100U + s_visit_bonus_ms;
        if (base > 60000U) {
            base = 60000U;
        }
    }
    if (s_config.adaptive && s_config.jitter_percent != 0) {
        uint32_t jitter = (base * s_config.jitter_percent) / 100U;
        if (jitter != 0) {
            int32_t delta = (int32_t)(rand() % (2U * jitter + 1U)) - (int32_t)jitter;
            int32_t value = (int32_t)base + delta;
            base = value < 0 ? 0 : (uint32_t)value;
        }
    }
    return base < POLLING_MIN_DWELL_MS ? POLLING_MIN_DWELL_MS : base;
}

static void polling_update_dwell(bool field_active, uint8_t slot) {
    s_current_dwell_field = field_active;
    s_current_dwell_ms = effective_dwell_ms(field_active, slot);
}

static uint32_t ticks_to_ms(uint32_t ticks) {
    return (uint32_t)((uint64_t)ticks * 1000U / APP_TIMER_TICKS(1000));
}

static void polling_stop(void) {
    s_running = false;
    g_polling_keep_awake = false;
}

void polling_init(bool field_wake) {
    polling_set_defaults();
    (void)polling_load_config();
    uint32_t now = app_timer_cnt_get();
    s_last_switch = now;
    s_in_cooldown = false;
    s_cooldown_start = 0;
    s_cycle_count = 0;
    s_woken_by_field = field_wake;
    s_manual_stop = false;
    s_timed_out = false;
    s_evt_field_on = false;
    s_evt_field_off = false;
    s_evt_hf_selected = false;
    s_evt_hf_activity = false;
    s_last_activity_tick = now;
    s_hit_count = 0;
    s_last_hit_slot = 0xFF;
    polling_reset_scales();
    polling_reset_visit(false);
    // The field wake-up itself counts as having seen a field
    s_last_field_tick = now;
    polling_update_dwell(false, tag_emulation_get_slot());
    // polling_tick() decides whether to run once the device is in tag mode
    polling_stop();
}

bool polling_load_config(void) {
    // Sized for the largest record we know, FDS copies whole words
    polling_config_t tmp __attribute__((aligned(4)));
    uint16_t length = sizeof(tmp);
    if (fds_read_sync(FDS_POLLING_CONFIG_FILE_ID, FDS_POLLING_CONFIG_RECORD_KEY, &length, (uint8_t *)&tmp)) {
        if (length == sizeof(tmp) && polling_config_valid(&tmp)) {
            memcpy(&s_config, &tmp, sizeof(s_config));
            return true;
        }
        if (length == ULC_POLLING_CONFIG_V2_SIZE && tmp.version == ULC_POLLING_CONFIG_V2_VERSION) {
            // Migrate a v2 record: keep its fields, new ones take their defaults
            polling_config_t mig __attribute__((aligned(4)));
            polling_set_defaults();
            memcpy(&mig, &s_config, sizeof(mig));
            memcpy(&mig, &tmp, ULC_POLLING_CONFIG_V2_COMMON);
            mig.version = ULC_POLLING_CONFIG_VERSION;
            if (polling_config_valid(&mig)) {
                memcpy(&s_config, &mig, sizeof(s_config));
                // Best effort, the migration simply runs again on the next boot if this fails
                (void)polling_save_config();
                return true;
            }
        }
    }
    // Missing, unknown version or wrong length: fall back to defaults
    polling_set_defaults();
    return false;
}

bool polling_save_config(void) {
    return fds_write_sync(FDS_POLLING_CONFIG_FILE_ID, FDS_POLLING_CONFIG_RECORD_KEY,
                          sizeof(s_config), &s_config);
}

const polling_config_t *polling_get_config(void) {
    return &s_config;
}

bool polling_set_config(const polling_config_t *config) {
    if (!polling_config_valid(config)) {
        return false;
    }
    // Persist first, only apply to RAM once the record is safely written
    polling_config_t tmp __attribute__((aligned(4)));
    memcpy(&tmp, config, sizeof(tmp));
    memset(tmp.reserved, 0, sizeof(tmp.reserved));
    if (!fds_write_sync(FDS_POLLING_CONFIG_FILE_ID, FDS_POLLING_CONFIG_RECORD_KEY, sizeof(tmp), &tmp)) {
        return false;
    }
    memcpy(&s_config, &tmp, sizeof(s_config));
    // An explicit config from the host restarts polling (still field wake-up only)
    s_last_switch = app_timer_cnt_get();
    s_in_cooldown = false;
    s_timed_out = false;
    s_manual_stop = false;
    s_hit_count = 0;
    s_last_hit_slot = 0xFF;
    s_evt_hf_selected = false;
    s_evt_hf_activity = false;
    polling_reset_scales();
    polling_reset_visit(g_is_tag_emulating);
    polling_stop();
    return true;
}

void polling_note_manual_switch(void) {
    s_last_switch = app_timer_cnt_get();
    s_manual_stop = true;
    polling_stop();
}

void polling_note_field_state(bool active) {
    if (active) {
        s_last_field_tick = app_timer_cnt_get();
        s_evt_field_on = true;
    } else {
        s_evt_field_off = true;
    }
}

void polling_note_hf_selected(void) {
    s_evt_hf_selected = true;
}

void polling_note_hf_activity(void) {
    s_last_activity_tick = app_timer_cnt_get();
    s_evt_hf_activity = true;
}

void polling_get_status(polling_status_t *status) {
    if (status == NULL) {
        return;
    }
    uint32_t remaining = 0;
    if (s_running) {
        uint32_t now = app_timer_cnt_get();
        uint32_t elapsed = app_timer_cnt_diff_compute(now, s_last_switch);
        uint32_t dwell_ticks = APP_TIMER_TICKS(s_current_dwell_ms);
        remaining = elapsed >= dwell_ticks ? 0 : dwell_ticks - elapsed;
        if (s_holding) {
            // Report what is left of the transaction hold instead
            uint32_t hold_ticks = APP_TIMER_TICKS(s_config.txn_hold_ms);
            uint32_t held = app_timer_cnt_diff_compute(now, s_txn_start);
            remaining = held >= hold_ticks ? 0 : hold_ticks - held;
        }
        if (s_in_cooldown) {
            uint32_t cd_elapsed = app_timer_cnt_diff_compute(now, s_cooldown_start);
            uint32_t cd_ticks = APP_TIMER_TICKS(s_config.cooldown_ms);
            uint32_t cd_remaining = cd_elapsed >= cd_ticks ? 0 : cd_ticks - cd_elapsed;
            if (cd_remaining > remaining) {
                remaining = cd_remaining;
            }
        }
    }
    status->enabled = s_config.enabled;
    status->running = s_running;
    status->current_slot = tag_emulation_get_slot();
    status->field_active = g_is_tag_emulating;
    status->remaining_ms = ticks_to_ms(remaining);
    status->cycle_count = s_cycle_count;
    status->woken_by_field = s_woken_by_field;
    if (s_running && s_in_cooldown) {
        status->phase = ULC_POLLING_PHASE_COOLDOWN;
    } else if (!g_is_tag_emulating) {
        status->phase = ULC_POLLING_PHASE_IDLE;
    } else if (s_running && s_holding) {
        status->phase = ULC_POLLING_PHASE_HOLD;
    } else {
        status->phase = ULC_POLLING_PHASE_FIELD;
    }
    status->hit_count = s_hit_count;
    status->last_hit_slot = s_last_hit_slot;
}

void polling_tick(void) {
    uint32_t now = app_timer_cnt_get();
    bool field = g_is_tag_emulating;

    // Consume the field events recorded by the interrupt handlers
    bool evt_on = s_evt_field_on;
    if (evt_on) {
        s_evt_field_on = false;
    }
    bool evt_off = s_evt_field_off;
    if (evt_off) {
        s_evt_field_off = false;
    }
    if (evt_off && !field) {
        s_in_cooldown = true;
        s_cooldown_start = now;
    }
    if (evt_on) {
        // A real field ends the no-field timeout
        s_timed_out = false;
        if (field) {
            s_in_cooldown = false;
        }
    }
    if (field) {
        s_last_field_tick = now;
    }

    if (!s_config.enabled || !s_woken_by_field || s_manual_stop || s_timed_out ||
            get_device_mode() != DEVICE_MODE_TAG) {
        polling_stop();
        return;
    }

    if (!s_running) {
        // Starting (again): open a fresh dwell and no-field window
        s_running = true;
        s_last_switch = now;
        s_last_field_tick = now;
        s_in_cooldown = false;
        s_evt_hf_selected = false;
        s_evt_hf_activity = false;
        polling_reset_visit(field);
        polling_update_dwell(field, tag_emulation_get_slot());
        if (s_config.keep_awake) {
            // sleep_timer_start() ignores calls while keep-awake is set, but
            // cannot cancel a timer that is already running.
            sleep_timer_stop();
        }
    }
    g_polling_keep_awake = s_config.keep_awake != 0;

    // Switching slots re-arms the sensors without a FIELD_LOST event, so we
    // must stop on our own if no reader shows up for a while.
    uint32_t timeout_ms = 2U * s_config.idle_dwell_ms;
    if (timeout_ms < ULC_POLLING_NO_FIELD_TIMEOUT_MS) {
        timeout_ms = ULC_POLLING_NO_FIELD_TIMEOUT_MS;
    }
    // Sample the counter after the tick: the ISR may have stored a value newer than 'now'
    uint32_t last_field = s_last_field_tick;
    uint32_t since_field = app_timer_cnt_diff_compute(app_timer_cnt_get(), last_field);
    if (!field && since_field >= APP_TIMER_TICKS(timeout_ms)) {
        NRF_LOG_INFO("No field for %d ms, polling stopped.", timeout_ms);
        s_timed_out = true;
        polling_stop();
        sleep_timer_start(SLEEP_DELAY_MS_FIELD_NFC_LOST);
        return;
    }

    if (evt_off || !field) {
        s_visit_field_lost = true;
    }

    // Consume the HF events recorded by the NFCT interrupt handler
    uint8_t cur_slot = tag_emulation_get_slot();
    if (s_evt_hf_activity) {
        s_evt_hf_activity = false;
        if (!s_visit_activity) {
            s_visit_activity = true;
            s_txn_start = s_last_activity_tick;
        }
    }
    if (s_evt_hf_selected) {
        s_evt_hf_selected = false;
        s_hit_count++;
        s_last_hit_slot = cur_slot;
        s_slot_scale[cur_slot] = 100;
        s_visit_hit = true;
        if (s_config.hit_bonus_ms > 0 && s_visit_bonus_ms == 0) {
            // Extend this visit once; polling_update_dwell() keeps it on a re-roll
            s_visit_bonus_ms = s_config.hit_bonus_ms;
            if (s_current_dwell_field) {
                s_current_dwell_ms += s_visit_bonus_ms;
                if (s_current_dwell_ms > 60000U) {
                    s_current_dwell_ms = 60000U;
                }
            }
        }
    }

    if (field != s_current_dwell_field) {
        polling_update_dwell(field, cur_slot);
    }

    if (s_in_cooldown) {
        if (app_timer_cnt_diff_compute(now, s_cooldown_start) < APP_TIMER_TICKS(s_config.cooldown_ms)) {
            return;
        }
        s_in_cooldown = false;
        if (!field) {
            // Back to no field: the next reader starts from scratch
            polling_reset_scales();
        }
    }

    uint32_t elapsed = app_timer_cnt_diff_compute(now, s_last_switch);
    if (elapsed < APP_TIMER_TICKS(s_current_dwell_ms)) {
        s_holding = false;
        return;
    }

    if (s_config.txn_hold_ms > 0 && field && s_visit_activity) {
        // Defer the switch while the reader is still talking to this slot.
        // Read the activity tick before the counter so it can not be newer.
        uint32_t last_activity = s_last_activity_tick;
        uint32_t t = app_timer_cnt_get();
        if (app_timer_cnt_diff_compute(t, last_activity) < APP_TIMER_TICKS(ULC_POLLING_TXN_GAP_MS) &&
                app_timer_cnt_diff_compute(t, s_txn_start) < APP_TIMER_TICKS(s_config.txn_hold_ms)) {
            s_holding = true;
            return;
        }
    }

    uint8_t old = cur_slot;
    uint8_t next = next_selected_slot(old);
    s_last_switch = now;
    if (s_config.miss_shrink_pct > 0 && field && !s_visit_field_lost && !s_visit_hit && slot_has_hf(old)) {
        // Field was there the whole visit but the reader never selected this slot
        uint32_t scale = (uint32_t)s_slot_scale[old] * (100U - s_config.miss_shrink_pct) / 100U;
        s_slot_scale[old] = scale < ULC_POLLING_MIN_SCALE_PCT ? ULC_POLLING_MIN_SCALE_PCT : (uint8_t)scale;
    }
    s_evt_hf_activity = false;
    s_evt_hf_selected = false;
    polling_reset_visit(field);
    if (next == old) {
        polling_update_dwell(field, old);
        return;
    }
    polling_update_dwell(field, next);

    tag_emulation_change_slot(next, true);
    apply_slot_change(old, next);
    s_cycle_count++;
}
