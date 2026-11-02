#include "polling.h"

#include <stdlib.h>
#include <string.h>

#include "app_timer.h"
#include "fds_ids.h"
#include "fds_util.h"
#include "rfid_main.h"
#include "tag_emulation.h"

#define NRF_LOG_MODULE_NAME polling
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
NRF_LOG_MODULE_REGISTER();

volatile bool g_polling_keep_awake = false;

static polling_config_t s_config;
static uint32_t s_last_switch;
static uint32_t s_cooldown_until;
static uint16_t s_cycle_count;
static bool s_running;
static uint8_t s_next_slot;
static bool s_field_wake;
static bool s_manual_triggered;

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
}

static bool polling_config_valid(const polling_config_t *cfg) {
    if (cfg == NULL || cfg->version != ULC_POLLING_CONFIG_VERSION) {
        return false;
    }
    if (cfg->idle_dwell_ms < 20 || cfg->idle_dwell_ms > 60000) {
        return false;
    }
    if (cfg->active_dwell_ms < 50 || cfg->active_dwell_ms > 60000) {
        return false;
    }
    if (cfg->cooldown_ms > 10000) {
        return false;
    }
    if (cfg->jitter_percent > 50) {
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
    return types.tag_hf != TAG_TYPE_UNDEFINED || types.tag_lf != TAG_TYPE_UNDEFINED;
}

static uint8_t next_selected_slot(uint8_t from) {
    uint16_t mask = s_config.slot_mask == 0 ? 0xFFFFU : s_config.slot_mask;
    for (uint8_t i = 1; i <= TAG_MAX_SLOT_NUM; i++) {
        uint8_t slot = (uint8_t)((from + i) % TAG_MAX_SLOT_NUM);
        if ((mask & (1U << slot)) != 0 && slot_usable(slot)) {
            return slot;
        }
    }
    return from;
}

static uint32_t effective_dwell_ms(bool field_active) {
    uint32_t base = field_active ? s_config.active_dwell_ms : s_config.idle_dwell_ms;
    if (!s_config.adaptive || s_config.jitter_percent == 0) {
        return base;
    }
    uint32_t jitter = (base * s_config.jitter_percent) / 100U;
    if (jitter == 0) {
        return base;
    }
    int32_t delta = (int32_t)(rand() % (2U * jitter + 1U)) - (int32_t)jitter;
    int32_t value = (int32_t)base + delta;
    return value < 50 ? 50U : (uint32_t)value;
}

void polling_init(bool field_wake) {
    polling_set_defaults();
    (void)polling_load_config();
    s_last_switch = app_timer_cnt_get();
    s_cooldown_until = 0;
    s_cycle_count = 0;
    s_field_wake = field_wake;
    s_manual_triggered = false;
    s_running = s_config.enabled && s_field_wake;
    s_next_slot = tag_emulation_get_slot();
    g_polling_keep_awake = s_running && s_config.keep_awake;
}

bool polling_load_config(void) {
    uint16_t length = sizeof(s_config);
    if (!fds_read_sync(FDS_POLLING_CONFIG_FILE_ID, FDS_POLLING_CONFIG_RECORD_KEY, &length, (uint8_t *)&s_config) ||
            length != sizeof(s_config) || !polling_config_valid(&s_config)) {
        polling_set_defaults();
        return false;
    }
    return true;
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
    memcpy(&s_config, config, sizeof(s_config));
    s_last_switch = app_timer_cnt_get();
    s_cooldown_until = 0;
    s_manual_triggered = false;
    s_running = s_config.enabled && s_field_wake;
    g_polling_keep_awake = s_running && s_config.keep_awake;
    return polling_save_config();
}

void polling_note_manual_switch(void) {
    s_last_switch = app_timer_cnt_get();
    s_manual_triggered = true;
    s_running = false;
    g_polling_keep_awake = false;
    s_next_slot = tag_emulation_get_slot();
}

void polling_note_field_state(bool active) {
    s_field_wake = active;
    if (active) {
        s_manual_triggered = false;
        s_last_switch = app_timer_cnt_get();
        s_cooldown_until = 0;
    } else {
        s_running = false;
        g_polling_keep_awake = false;
    }
}

void polling_get_status(polling_status_t *status) {
    if (status == NULL) {
        return;
    }
    uint32_t now = app_timer_cnt_get();
    uint32_t dwell = effective_dwell_ms(g_is_tag_emulating);
    uint32_t elapsed = app_timer_cnt_diff_compute(now, s_last_switch);
    uint32_t dwell_ticks = APP_TIMER_TICKS(dwell);
    uint32_t remaining = elapsed >= dwell_ticks ? 0 : dwell_ticks - elapsed;
    status->enabled = s_config.enabled;
    status->running = s_running;
    status->current_slot = tag_emulation_get_slot();
    status->field_active = g_is_tag_emulating;
    status->remaining_ms = (uint16_t)((uint64_t)remaining * 1000U / APP_TIMER_TICKS(1000));
    status->cycle_count = s_cycle_count;
    (void)s_next_slot;
}

void polling_tick(void) {
    if (!s_config.enabled || !s_field_wake || s_manual_triggered) {
        s_running = false;
        g_polling_keep_awake = false;
        return;
    }

    if (get_device_mode() != DEVICE_MODE_TAG) {
        s_running = false;
        return;
    }

    s_running = true;
    g_polling_keep_awake = s_config.keep_awake != 0;

    uint32_t now = app_timer_cnt_get();
    if (s_cooldown_until != 0 && (int32_t)(now - s_cooldown_until) < 0) {
        return;
    }
    s_cooldown_until = 0;

    bool field = g_is_tag_emulating;
    uint32_t dwell = effective_dwell_ms(field);
    uint32_t elapsed = app_timer_cnt_diff_compute(now, s_last_switch);
    if (elapsed < APP_TIMER_TICKS(dwell)) {
        return;
    }

    uint8_t old = tag_emulation_get_slot();
    uint8_t next = next_selected_slot(old);
    if (next == old) {
        s_last_switch = now;
        return;
    }

    tag_emulation_change_slot(next, true);
    apply_slot_change(old, next);
    s_next_slot = next;
    s_last_switch = now;
    s_cycle_count++;
}
