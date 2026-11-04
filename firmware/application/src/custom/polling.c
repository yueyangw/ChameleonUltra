#include "polling.h"

#include <stdlib.h>
#include <string.h>

#include "app_timer.h"
#include "fds_ids.h"
#include "fds_util.h"
#include "nfc_14a.h"
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

extern volatile uint32_t g_slot_prof[6];

// FDS writes whole words straight from this buffer, keep it word aligned.
static polling_config_t s_config __attribute__((aligned(4)));

// ---- Lab support: event ring buffer, filled from interrupts and the main loop ----
#define POLLING_LOG_SIZE 1024
static polling_log_entry_t s_log[POLLING_LOG_SIZE];
static volatile uint32_t s_log_head;
static volatile uint8_t s_lab_flags;

static void polling_log(uint8_t event, uint8_t slot, uint16_t arg) {
    if (!(s_lab_flags & ULC_POLLING_LAB_LOG)) {
        return;
    }
    uint32_t idx = __atomic_fetch_add(&s_log_head, 1, __ATOMIC_RELAXED);
    polling_log_entry_t *e = &s_log[idx % POLLING_LOG_SIZE];
    e->tick = app_timer_cnt_get();
    e->event = event;
    e->slot = slot;
    e->arg = arg;
}

void polling_lab_set(const polling_lab_t *lab) {
    s_lab_flags = lab->flags;
    if (lab->clear_log) {
        s_log_head = 0;
    }
}

void polling_lab_get(polling_lab_t *lab) {
    lab->flags = s_lab_flags;
    lab->engine = 1;
    lab->clear_log = 0;
    lab->reserved = 0;
}

uint32_t polling_log_total(void) {
    return s_log_head;
}

uint16_t polling_log_read(uint32_t from, uint16_t max, polling_log_entry_t *out) {
    uint32_t head = s_log_head;
    uint32_t oldest = head > POLLING_LOG_SIZE ? head - POLLING_LOG_SIZE : 0;
    if (from < oldest) {
        from = oldest;
    }
    uint16_t n = 0;
    while (from < head && n < max) {
        out[n++] = s_log[from % POLLING_LOG_SIZE];
        from++;
    }
    return n;
}

// ---- Run state ----
static uint32_t s_last_switch;
static uint32_t s_idle_dwell_ms;      // rotation time of the current slot while no field is present
static bool s_in_cooldown;
static uint32_t s_cooldown_start;
static uint16_t s_cycle_count;
static bool s_running;
static bool s_woken_by_field;         // set once at boot, fixed for this power cycle
static bool s_manual_stop;            // sticky until the host pushes a new config
static bool s_timed_out;              // no field seen for too long
static uint16_t s_hit_count;
static uint8_t s_last_hit_slot = 0xFF;
static uint16_t s_bad_mask;           // slots skipped once after failed auths
static bool s_visit_auth_ok;          // a MF1 auth succeeded during the visit
static bool s_advance_pending;        // next slot as soon as the field is back (reset in mid transaction)
static bool s_leave_pending;          // next slot as soon as the field is back (sticky sessions off)
static uint32_t s_field_off_tick;     // when the field was last lost (main loop view)

// Written from the NFCT / LPCOMP interrupt handlers, consumed by polling_tick().
static volatile bool s_evt_field_on;
static volatile bool s_evt_field_off;
static volatile uint32_t s_last_field_tick;
static volatile bool s_evt_hf_selected;
static volatile bool s_evt_auth_ok;
static volatile bool s_sess_auth_fail;        // the last MF1 auth failed (cleared by a new selection or a success)
static volatile uint32_t s_auth_fail_tick;

// Reader poll period, learned from REQA/WUPA intervals in the NFCT interrupt
static volatile uint32_t s_last_poll_tick;
static volatile bool s_poll_seen;
static volatile uint16_t s_poll_samples[ULC_POLLING_POLL_SAMPLES];
static volatile uint8_t s_poll_count;
static volatile uint8_t s_poll_next;
static volatile bool s_evt_poll;
static uint16_t s_poll_period_ms;     // median of the samples, 0: unknown

// Per visit counters (a visit is the time a slot is presented), written by the NFCT interrupt
static volatile uint8_t s_v_polls;            // REQA/WUPA seen
static volatile uint8_t s_v_selects;          // completed selections
static volatile uint8_t s_v_halts;            // HALTs of the card
static volatile uint8_t s_v_selects_at_halt;  // s_v_selects at the last HALT
static volatile uint8_t s_fail_selects;       // s_v_selects at the last failed auth
static volatile uint8_t s_v_auth_fails;       // failed MF1 auths in this visit
static volatile uint8_t s_v_polls_since_fail; // REQA/WUPA seen since the last failed auth
static volatile uint32_t s_v_halt_tick;
static volatile uint32_t s_v_interact_tick;   // last frame longer than a REQA/WUPA (also SELECT / auth / HALT)
static volatile uint32_t s_v_first_poll_tick; // first REQA/WUPA of the visit
static uint8_t s_v_unhalts;                   // HALTed card re-armed in this visit
static uint8_t s_v_sessions;                  // reader discovery sessions (field bursts) seen on this slot

static uint32_t ticks_to_ms(uint32_t ticks) {
    return (uint32_t)((uint64_t)ticks * 1000U / APP_TIMER_TICKS(1000));
}

static uint32_t ms_since(uint32_t tick) {
    return ticks_to_ms(app_timer_cnt_diff_compute(app_timer_cnt_get(), tick));
}

// ---- Configuration ----
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
    s_config.done_quiet_ms = ULC_POLLING_DEFAULT_DONE_QUIET_MS;
    s_config.patience_ms = ULC_POLLING_DEFAULT_PATIENCE_MS;
    s_config.sticky_sessions = ULC_POLLING_DEFAULT_STICKY_SESSIONS;
    s_config.fail_poll_quiet_ms = ULC_POLLING_DEFAULT_FAIL_POLL_QUIET_MS;
    s_config.represent_count = ULC_POLLING_DEFAULT_REPRESENT;
    s_config.auth_fail_count = ULC_POLLING_DEFAULT_FAIL_COUNT;
    s_config.lock_quiet_100ms = ULC_POLLING_DEFAULT_LOCK_QUIET_100MS;
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
    if (cfg->cooldown_ms > 10000 || cfg->jitter_percent > 50) {
        return false;
    }
    if (cfg->done_quiet_ms > 5000 || cfg->patience_ms > 10000 || cfg->fail_poll_quiet_ms > 5000) {
        return false;
    }
    if (cfg->sticky_sessions > 20 || cfg->represent_count > 10 || cfg->auth_fail_count > 10) {
        return false;
    }
    if (cfg->lock_quiet_100ms < 1 || cfg->lock_quiet_100ms > 100) {
        return false;
    }
    return true;
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
        bool legacy = (length == ULC_POLLING_CONFIG_V2_SIZE && tmp.version == ULC_POLLING_CONFIG_V2_VERSION) ||
                      (length == sizeof(tmp) && tmp.version >= ULC_POLLING_CONFIG_FIRST_SAME_SIZE &&
                       tmp.version <= ULC_POLLING_CONFIG_LAST_LEGACY);
        if (legacy) {
            // Older records share the leading fields, the waits of the new engine take their defaults
            polling_set_defaults();
            memcpy(&s_config, &tmp, ULC_POLLING_CONFIG_LEGACY_COMMON);
            s_config.version = ULC_POLLING_CONFIG_VERSION;
            if (length == sizeof(tmp) && tmp.version >= 4 && tmp.represent_count <= 10) {
                s_config.represent_count = tmp.represent_count;
            }
            if (polling_config_valid(&s_config)) {
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

// ---- Slot choice ----
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

// The next slot to present. A slot whose reader auth failed is skipped once and then gets another
// chance: a phone that probes keys fails on slots whose keys it does not know, but not on all of them.
static uint8_t next_selected_slot(uint8_t from) {
    if (s_config.order == 1) {
        for (int pass = 0; pass < 2; pass++) {
            uint8_t candidates[TAG_MAX_SLOT_NUM];
            uint8_t count = 0;
            for (uint8_t slot = 0; slot < TAG_MAX_SLOT_NUM; slot++) {
                if (slot != from && (pass == 1 || (s_bad_mask & (1U << slot)) == 0) && slot_selected(slot)) {
                    candidates[count++] = slot;
                }
            }
            if (count != 0) {
                return candidates[rand() % count];
            }
        }
        return from;
    }
    for (uint8_t i = 1; i <= TAG_MAX_SLOT_NUM; i++) {
        uint8_t slot = (uint8_t)((from + i) % TAG_MAX_SLOT_NUM);
        if (!slot_selected(slot)) {
            continue;
        }
        if ((s_bad_mask & (1U << slot)) != 0) {
            s_bad_mask &= (uint16_t)~(1U << slot);
            continue;
        }
        return slot;
    }
    return from;
}

// ---- Visit / session bookkeeping ----
static void polling_clear_hf_events(void) {
    s_evt_hf_selected = false;
    s_evt_auth_ok = false;
}

// The reader came back after a pause (or a slot was just presented): judge it afresh.
static void polling_reset_counters(void) {
    s_v_polls = 0;
    s_v_selects = 0;
    s_v_halts = 0;
    s_v_selects_at_halt = 0;
    s_fail_selects = 0;
    s_v_auth_fails = 0;
    s_v_polls_since_fail = 0;
    s_v_unhalts = 0;
    s_sess_auth_fail = false;
    s_v_interact_tick = app_timer_cnt_get();
}

static void polling_reset_visit(void) {
    polling_reset_counters();
    s_visit_auth_ok = false;
    s_v_sessions = 0;
}

static void polling_reset_poll_period(void) {
    s_poll_seen = false;
    s_poll_count = 0;
    s_poll_next = 0;
    s_evt_poll = false;
    s_poll_period_ms = 0;
}

static void polling_update_poll_period(void) {
    uint16_t sorted[ULC_POLLING_POLL_SAMPLES];
    uint8_t n = s_poll_count;
    if (n < ULC_POLLING_POLL_MIN_SAMPLES) {
        s_poll_period_ms = 0;
        return;
    }
    for (uint8_t i = 0; i < n; i++) {
        sorted[i] = s_poll_samples[i];
    }
    // Insertion sort, at most 8 samples
    for (uint8_t i = 1; i < n; i++) {
        uint16_t v = sorted[i];
        uint8_t j = i;
        while (j > 0 && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    s_poll_period_ms = sorted[n / 2];
}

static void polling_roll_idle_dwell(void) {
    uint32_t base = s_config.idle_dwell_ms;
    if (s_config.adaptive && s_config.jitter_percent != 0) {
        uint32_t jitter = (base * s_config.jitter_percent) / 100U;
        if (jitter != 0) {
            int32_t delta = (int32_t)(rand() % (2U * jitter + 1U)) - (int32_t)jitter;
            int32_t value = (int32_t)base + delta;
            base = value < 0 ? 0 : (uint32_t)value;
        }
    }
    s_idle_dwell_ms = base < POLLING_MIN_DWELL_MS ? POLLING_MIN_DWELL_MS : base;
}

static void polling_stop(void) {
    if (s_running) {
        polling_log(PLOG_STOP, tag_emulation_get_slot(), 0);
    }
    s_running = false;
    g_polling_keep_awake = false;
}

// Present `target` now: swap the emulated slot if needed and start a fresh visit.
static void polling_present(uint8_t target, uint32_t now, bool field) {
    uint8_t old = tag_emulation_get_slot();
    s_poll_seen = false; // the next REQA interval is no poll period
    polling_clear_hf_events();
    polling_reset_visit();
    s_last_switch = now;
    polling_roll_idle_dwell();
    if (target == old) {
        return;
    }
    if (field && g_is_tag_emulating) {
        // Reader present: keep the sensors running, see tag_emulation_change_slot_live()
        g_slot_prof[0] = app_timer_cnt_get();
        tag_emulation_change_slot_live(target);
        for (uint8_t i = 1; i < 6; i++) {
            g_slot_prof[i] = app_timer_cnt_get();
        }
    } else {
        tag_emulation_change_slot(target, true);
    }
    uint32_t t_apply = app_timer_cnt_get();
    apply_slot_change_fast(old, target);
    uint32_t t_done = app_timer_cnt_get();
    s_cycle_count++;
    polling_log(PLOG_SWITCH, target, old);
    if (s_lab_flags & ULC_POLLING_LAB_LOG) {
        for (uint8_t i = 0; i < 5; i++) {
            polling_log(PLOG_PROF, i, (uint16_t)app_timer_cnt_diff_compute(g_slot_prof[i + 1], g_slot_prof[i]));
        }
        polling_log(PLOG_PROF, 5, (uint16_t)app_timer_cnt_diff_compute(t_done, t_apply));
    }
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
    s_advance_pending = false;
    s_leave_pending = false;
    s_field_off_tick = now;
    polling_clear_hf_events();
    s_hit_count = 0;
    s_last_hit_slot = 0xFF;
    s_bad_mask = 0;
    polling_reset_visit();
    polling_reset_poll_period();
    // The field wake-up itself counts as having seen a field
    s_last_field_tick = now;
    polling_roll_idle_dwell();
    // polling_tick() decides whether to run once the device is in tag mode
    polling_stop();
}

bool polling_set_config(const polling_config_t *config) {
    if (!polling_config_valid(config)) {
        return false;
    }
    // Persist first, only apply to RAM once the record is safely written
    polling_config_t tmp __attribute__((aligned(4)));
    memcpy(&tmp, config, sizeof(tmp));
    if (!fds_write_sync(FDS_POLLING_CONFIG_FILE_ID, FDS_POLLING_CONFIG_RECORD_KEY, sizeof(tmp), &tmp)) {
        return false;
    }
    memcpy(&s_config, &tmp, sizeof(s_config));
    // An explicit config from the host restarts polling (still field wake-up only)
    s_last_switch = app_timer_cnt_get();
    s_in_cooldown = false;
    s_timed_out = false;
    s_manual_stop = false;
    s_advance_pending = false;
    s_leave_pending = false;
    s_hit_count = 0;
    s_last_hit_slot = 0xFF;
    s_bad_mask = 0;
    polling_clear_hf_events();
    polling_reset_visit();
    polling_roll_idle_dwell();
    polling_stop();
    return true;
}

void polling_note_manual_switch(void) {
    s_last_switch = app_timer_cnt_get();
    s_manual_stop = true;
    polling_stop();
}

// ---- Hooks called from the NFC/LF interrupt handlers ----
void polling_note_field_state(bool active) {
    polling_log(active ? PLOG_FIELD_ON : PLOG_FIELD_OFF, tag_emulation_get_slot(), 0);
    if (active) {
        s_last_field_tick = app_timer_cnt_get();
        s_evt_field_on = true;
    } else {
        s_evt_field_off = true;
    }
}

void polling_note_hf_selected(void) {
    polling_log(PLOG_SELECT, tag_emulation_get_slot(), 0);
    if (s_v_selects < 255) {
        s_v_selects++;
    }
    s_v_interact_tick = app_timer_cnt_get();
    s_sess_auth_fail = false; // a new selection starts a new auth attempt
    s_evt_hf_selected = true;
}

void polling_note_hf_activity(void) {
}

void polling_note_hf_interaction(void) {
    s_v_interact_tick = app_timer_cnt_get();
}

void polling_note_hf_halt(void) {
    polling_log(PLOG_HALT, tag_emulation_get_slot(), 0);
    if (s_v_halts < 255) {
        s_v_halts++;
    }
    s_v_selects_at_halt = s_v_selects;
    s_v_halt_tick = app_timer_cnt_get();
    s_v_interact_tick = s_v_halt_tick;
}

void polling_note_mf1_auth(bool ok) {
    polling_log(ok ? PLOG_AUTH_OK : PLOG_AUTH_FAIL, tag_emulation_get_slot(), 0);
    s_v_interact_tick = app_timer_cnt_get();
    if (ok) {
        s_sess_auth_fail = false;
        s_evt_auth_ok = true;
        return;
    }
    s_fail_selects = s_v_selects;
    s_v_polls_since_fail = 0;
    if (s_v_auth_fails < 255) {
        s_v_auth_fails++;
    }
    s_auth_fail_tick = s_v_interact_tick;
    s_sess_auth_fail = true;
}

bool polling_wants_nfct_reset(void) {
    return s_config.enabled != 0;
}

void polling_note_hf_poll(void) {
    uint32_t now = app_timer_cnt_get();
    if (s_v_polls == 0) {
        s_v_first_poll_tick = now;
    }
    if (s_v_polls < 255) {
        s_v_polls++;
    }
    if (s_v_auth_fails != 0 && s_v_polls_since_fail < 255) {
        s_v_polls_since_fail++;
    }
    if (s_poll_seen) {
        uint32_t ms = ticks_to_ms(app_timer_cnt_diff_compute(now, s_last_poll_tick));
        if (ms >= ULC_POLLING_POLL_MIN_MS && ms <= ULC_POLLING_POLL_MAX_MS) {
            polling_log(PLOG_POLL, tag_emulation_get_slot(), (uint16_t)ms);
            s_poll_samples[s_poll_next] = (uint16_t)ms;
            s_poll_next = (uint8_t)((s_poll_next + 1) % ULC_POLLING_POLL_SAMPLES);
            if (s_poll_count < ULC_POLLING_POLL_SAMPLES) {
                s_poll_count++;
            }
            s_evt_poll = true;
        }
    }
    s_last_poll_tick = now;
    s_poll_seen = true;
}

void polling_get_status(polling_status_t *status) {
    if (status == NULL) {
        return;
    }
    uint32_t remaining = 0;
    if (s_running && !g_is_tag_emulating) {
        uint32_t elapsed = app_timer_cnt_diff_compute(app_timer_cnt_get(), s_last_switch);
        uint32_t dwell_ticks = APP_TIMER_TICKS(s_idle_dwell_ms);
        remaining = elapsed >= dwell_ticks ? 0 : dwell_ticks - elapsed;
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
    } else if (s_running && s_visit_auth_ok) {
        status->phase = ULC_POLLING_PHASE_HOLD;
    } else {
        status->phase = ULC_POLLING_PHASE_FIELD;
    }
    status->hit_count = s_hit_count;
    status->last_hit_slot = s_last_hit_slot;
    status->bad_mask = s_bad_mask;
    status->represents_left = 0;
    status->poll_period_ms = s_poll_period_ms;
}

// ---- The engine ----

// Decide from what the reader does with the presented slot, not from timers.
static void engine_step(uint32_t now, uint8_t cur) {
    uint32_t period = s_poll_period_ms ? s_poll_period_ms : ULC_POLLING_DEFAULT_PERIOD_MS;
    uint8_t reads_per_slot = (uint8_t)(1U + s_config.represent_count);
    uint8_t next;
    uint8_t reason = 0;

    if (s_leave_pending) {
        // The reader paused its field and sticky sessions are off: the next slot gets its turn
        s_leave_pending = false;
        next = next_selected_slot(cur);
        if (next != cur) {
            polling_log(PLOG_DECIDE, cur, PDEC_SESSIONS);
            polling_present(next, now, true);
            return;
        }
    }
    if (s_advance_pending) {
        // The reader reset its field in the middle of a transaction with the previous slot
        s_advance_pending = false;
        if (ms_since(s_field_off_tick) > ULC_POLLING_NEW_SESSION_MS) {
            return; // too long ago to be a reset of the same reader
        }
        next = next_selected_slot(cur);
        if (next != cur) {
            polling_log(PLOG_DECIDE, cur, PDEC_RF_RESET);
            polling_present(next, now, true);
            return;
        }
    }

    uint32_t quiet_ms = ms_since(s_v_interact_tick);

    if (s_config.sticky_sessions != 0 && s_v_sessions > s_config.sticky_sessions && !s_visit_auth_ok) {
        // The same reader came back several times and never got anywhere with this slot
        reason = PDEC_SESSIONS;
    } else if (s_v_polls == 0 && s_v_selects == 0) {
        // A field without any REQA/WUPA: not a polling reader we can read the pace of. Rotate slowly
        // so a reader that wakes up late still meets every slot.
        if (ms_since(s_last_switch) < s_config.active_dwell_ms) {
            return;
        }
        reason = PDEC_SILENT;
    } else {
        bool fail_leave = false;
        if (s_config.auth_fail_count > 0 && !s_visit_auth_ok && s_v_auth_fails != 0) {
            if (s_v_auth_fails >= s_config.auth_fail_count) {
                // Let the exchange that just failed finish before the card disappears
                fail_leave = s_sess_auth_fail && ms_since(s_auth_fail_tick) >= ULC_POLLING_FAIL_SETTLE_MS;
            } else if (s_v_polls_since_fail >= 2 && s_v_selects == s_fail_selects) {
                // It looks for other cards instead of working on this one
                fail_leave = quiet_ms >= s_config.fail_poll_quiet_ms;
            } else {
                // Silent after the failure, e.g. a phone passing the tag to an app: be patient
                fail_leave = quiet_ms >= s_config.patience_ms;
            }
        }
        if (fail_leave) {
            s_bad_mask |= (uint16_t)(1U << cur);
            reason = PDEC_AUTH_FAIL;
        } else if (s_visit_auth_ok) {
            uint32_t lock_quiet = 100U * s_config.lock_quiet_100ms;
            if (lock_quiet < 4U * period) {
                lock_quiet = 4U * period;
            }
            if (quiet_ms < lock_quiet) {
                return; // the reader works with this slot
            }
            reason = PDEC_LOCK_IDLE;
        } else if (s_v_halts != 0) {
            if (s_v_halts >= reads_per_slot) {
                uint32_t need = s_v_auth_fails != 0 ? s_config.patience_ms : s_config.done_quiet_ms;
                if (quiet_ms < need) {
                    return; // still going at it (a phone re-reads over and over) or waiting on an app
                }
                reason = PDEC_READS_DONE;
            } else {
                uint32_t wait = 2U * period;
                if (wait < 60U) {
                    wait = 60U;
                }
                if (s_v_selects != s_v_selects_at_halt || ms_since(s_v_halt_tick) < wait) {
                    return;
                }
                // A HALTed card only answers WUPA; a reader that polls with REQA would never see
                // it again, so let the card come back for the next read.
                if (s_v_unhalts >= 2) {
                    reason = PDEC_READS_DONE;
                } else {
                    s_v_unhalts++;
                    s_v_halt_tick = app_timer_cnt_get();
                    nfc_tag_14a_set_state(NFC_TAG_STATE_14A_IDLE);
                    polling_log(PLOG_DECIDE, cur, PDEC_UNHALT);
                    return;
                }
            }
        } else if (s_v_selects != 0) {
            uint32_t stall = 5U * period;
            if (stall < 200U) {
                stall = 200U;
            }
            if (quiet_ms < stall) {
                return;
            }
            reason = PDEC_STALLED;
        } else {
            uint32_t ignored = ULC_POLLING_IGNORED_POLLS * period;
            if (ignored < ULC_POLLING_IGNORED_MIN_MS) {
                ignored = ULC_POLLING_IGNORED_MIN_MS;
            }
            if (s_v_polls < ULC_POLLING_IGNORED_POLLS || ms_since(s_v_first_poll_tick) < ignored) {
                return;
            }
            reason = PDEC_IGNORED;
        }
    }

    next = next_selected_slot(cur);
    polling_log(PLOG_DECIDE, cur, reason);
    if (next == cur) {
        // Nothing else to switch to: begin a fresh visit of the same slot
        polling_clear_hf_events();
        polling_reset_visit();
        s_last_switch = now;
        return;
    }
    polling_present(next, now, true);
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
        s_field_off_tick = now;
        if (s_v_selects != 0 && s_v_halts == 0 && !s_visit_auth_ok) {
            s_advance_pending = true;
        }
    }
    if (evt_on) {
        // A real field ends the no-field timeout
        s_timed_out = false;
        if (field) {
            s_in_cooldown = false;
        }
        uint32_t off_ms = ticks_to_ms(app_timer_cnt_diff_compute(now, s_field_off_tick));
        if (s_running && field && off_ms >= ULC_POLLING_NEW_SESSION_MS &&
                (s_v_selects != 0 || s_visit_auth_ok || s_v_sessions != 0)) {
            // The reader pauses its field and comes back (a phone does so every 2 s): judge it
            // afresh and keep the slot it worked on, for a limited number of sessions.
            polling_reset_counters();
            if (s_config.sticky_sessions == 0) {
                s_leave_pending = !s_visit_auth_ok;
            } else if (s_v_sessions < 255) {
                s_v_sessions++;
            }
        }
        s_v_interact_tick = now;
        s_last_switch = now; // the silent-reader rotation counts from when the field came
    }
    if (field) {
        s_last_field_tick = now;
    }

    bool trigger_ok = s_woken_by_field || (s_lab_flags & ULC_POLLING_LAB_ANY_FIELD);
    if (!s_config.enabled || !trigger_ok || s_manual_stop || s_timed_out ||
            get_device_mode() != DEVICE_MODE_TAG) {
        polling_stop();
        return;
    }

    if (!s_running) {
        // Starting (again): open a fresh dwell and no-field window
        polling_log(PLOG_START, tag_emulation_get_slot(), 0);
        s_running = true;
        s_last_switch = now;
        s_last_field_tick = now;
        s_in_cooldown = false;
        s_bad_mask = 0;
        s_advance_pending = false;
        s_leave_pending = false;
        polling_reset_poll_period();
        polling_clear_hf_events();
        polling_reset_visit();
        polling_roll_idle_dwell();
        if (s_config.keep_awake) {
            // sleep_timer_start() ignores calls while keep-awake is set, but
            // cannot cancel a timer that is already running.
            sleep_timer_stop();
        }
    }
    g_polling_keep_awake = s_config.keep_awake != 0;

    // Sample the counter after the tick: the ISR may have stored a value newer than 'now'
    uint32_t since_field = app_timer_cnt_diff_compute(app_timer_cnt_get(), s_last_field_tick);

    // Switching slots re-arms the sensors without a FIELD_LOST event, so we
    // must stop on our own if no reader shows up for a while.
    uint32_t timeout_ms = 2U * s_config.idle_dwell_ms;
    if (timeout_ms < ULC_POLLING_NO_FIELD_TIMEOUT_MS) {
        timeout_ms = ULC_POLLING_NO_FIELD_TIMEOUT_MS;
    }
    if (!field && since_field >= APP_TIMER_TICKS(1000)) {
        s_advance_pending = false; // a different reader by the time the field is back
        s_leave_pending = false;
        s_bad_mask = 0;
    }
    if (!field && since_field >= APP_TIMER_TICKS(timeout_ms)) {
        NRF_LOG_INFO("No field for %d ms, polling stopped.", timeout_ms);
        s_timed_out = true;
        polling_stop();
        sleep_timer_start(SLEEP_DELAY_MS_FIELD_NFC_LOST);
        return;
    }

    // Consume the events recorded by the NFCT interrupt handler
    uint8_t cur_slot = tag_emulation_get_slot();
    if (s_evt_hf_selected) {
        s_evt_hf_selected = false;
        s_hit_count++;
        s_last_hit_slot = cur_slot;
    }
    if (s_evt_auth_ok) {
        s_evt_auth_ok = false;
        s_visit_auth_ok = true;
    }
    if (s_evt_poll) {
        s_evt_poll = false;
        polling_update_poll_period();
    }

    if (s_in_cooldown) {
        if (app_timer_cnt_diff_compute(now, s_cooldown_start) < APP_TIMER_TICKS(s_config.cooldown_ms)) {
            return;
        }
        s_in_cooldown = false;
        if (!field) {
            // Back to no field: the next reader starts from scratch
            polling_reset_poll_period();
        }
    }

    if (field) {
        engine_step(now, cur_slot);
        return;
    }

    // No field. A reader that worked on this slot a moment ago and paused its field will most
    // likely look for the same tag again, so do not wander off.
    if (s_config.sticky_sessions != 0 && (s_v_selects != 0 || s_visit_auth_ok || s_v_sessions != 0) &&
            ticks_to_ms(app_timer_cnt_diff_compute(now, s_field_off_tick)) < ULC_POLLING_STICKY_HOLD_MS) {
        s_last_switch = now;
        return;
    }
    if (s_lab_flags & ULC_POLLING_LAB_HOLD_IDLE) {
        s_last_switch = now;
        return;
    }

    // Rotate through the slots so a reader that arrives meets each of them
    if (app_timer_cnt_diff_compute(now, s_last_switch) < APP_TIMER_TICKS(s_idle_dwell_ms)) {
        return;
    }
    uint8_t next = next_selected_slot(cur_slot);
    if (next == cur_slot) {
        s_last_switch = now;
        polling_roll_idle_dwell();
        return;
    }
    polling_present(next, now, false);
}
