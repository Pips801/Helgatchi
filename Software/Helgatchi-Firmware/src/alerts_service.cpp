#include "alerts_service.h"
#include "event_payload.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <string.h>

AlertsService g_alerts;

// ---------------------------------------------------------------------------
// RTC slow-memory shadow of the alert store.
//
// ESP32-S3 RTC slow memory is preserved across deep sleep and software
// resets, but is cleared on a power-on (cold) reset. That matches what we
// want: alerts survive sleep/wake cycles (a deep-sleep wake re-enters
// setup() like a reboot, but the data is still here), but clear on actual
// power-cycle.
//
// We use a magic word so we can tell "valid data in RTC" from "RTC was
// zero-initialized by the hardware on cold boot." Synced after every
// mutating call (raise / ack / clearAll).
// ---------------------------------------------------------------------------

// Bump the last digit whenever AlertRecord's layout changes — stale RTC
// bytes from an older layout must not be reinterpreted as records.
static constexpr uint32_t RTC_MAGIC = 0xA1E47ED2;  // "ALERTED2"

RTC_DATA_ATTR static uint32_t    _rtc_magic;
RTC_DATA_ATTR static AlertRecord _rtc_records[AlertsService::MAX_ALERTS];
RTC_DATA_ATTR static uint8_t     _rtc_count;
RTC_DATA_ATTR static uint16_t    _rtc_next_id;

void AlertsService::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(CMD_ALERT_ACK, this);
    // CMD_ALERT_SNOOZE is reserved for a future "silence for N seconds"
    // behavior; not wired yet.

    // Restore from RTC slow memory if the magic word indicates valid data.
    // On power-on the magic is zero (hardware-cleared); on deep-sleep wake
    // it's still RTC_MAGIC from the last save.
    if (_rtc_magic == RTC_MAGIC) {
        memcpy(_records, _rtc_records, sizeof(_records));
        _count   = _rtc_count;
        _next_id = _rtc_next_id;
        // Pre-sleep millis() values are stale (millis restarts at 0 after
        // deep-sleep wake). Reset all timestamps to "now" so the time-ago
        // display starts fresh rather than showing huge underflowed values.
        const uint32_t now = millis();
        for (uint8_t i = 0; i < _count; i++) {
            _records[i].first_seen_ms = now;
            _records[i].last_seen_ms  = now;
        }
        _syncToRTC();   // persist the timestamp reset
    }
}

void AlertsService::_syncToRTC() {
    memcpy(_rtc_records, _records, sizeof(_rtc_records));
    _rtc_count   = _count;
    _rtc_next_id = _next_id;
    _rtc_magic   = RTC_MAGIC;
}

uint16_t AlertsService::raise(const char* title,
                              AlertType        type,
                              HapticPatternId  vibe,
                              LedPatternId     led,
                              const char*      identifier,
                              int8_t           rssi) {
    if (!title || !title[0]) return INVALID_ALERT;

    const char* ident = identifier ? identifier : "";

    // Dedup against existing record with matching (type, identifier).
    if (ident[0]) {
        int idx = _findIndexByDedup(type, ident);
        if (idx >= 0) {
            AlertRecord& r = _records[idx];
            // A present device re-fires its rule on every advertisement —
            // several times a second per device. Posting an UPDATED per
            // sighting can fill the event queue within one loop iteration
            // (dispatch drains once per loop), dropping unrelated events —
            // including another alert's RAISED. The only consumer refreshes
            // a 1 Hz time label, so emit at most once per second per record.
            const uint32_t now  = millis();
            const bool     emit = (now - r.last_seen_ms) >= 1000;
            r.last_seen_ms = now;
            if (r.seen_count < UINT16_MAX) r.seen_count++;
            if (rssi != INT8_MIN) r.rssi = rssi;
            _syncToRTC();
            if (emit) _emit(EV_ALERT_UPDATED, r.id);
            return r.id;
        }
    }

    // New alert. Drop if capacity is full — preserve unacked history.
    if (_count >= MAX_ALERTS) return INVALID_ALERT;

    AlertRecord& r = _records[_count++];
    r.id = _next_id++;
    if (_next_id == INVALID_ALERT) _next_id = 1;     // skip 0 on wrap
    r.type           = type;
    r.vibe           = vibe;
    r.led            = led;
    r.rssi           = rssi;
    r.first_seen_ms  = millis();
    r.last_seen_ms   = r.first_seen_ms;
    r.seen_count     = 1;
    strncpy(r.title,      title, sizeof(r.title) - 1);
    r.title[sizeof(r.title) - 1] = '\0';
    strncpy(r.identifier, ident, sizeof(r.identifier) - 1);
    r.identifier[sizeof(r.identifier) - 1] = '\0';

    _syncToRTC();
    // Side effects (vibe, LED, screen wake) come from VibeService,
    // LedService, and PowerManager subscribing to EV_ALERT_RAISED — each
    // queries find(alert_id) for the per-alert pattern and applies its own
    // SKEY_ALERT_* gating. AlertsService stays UI-only.
    _emit(EV_ALERT_RAISED, r.id);
    return r.id;
}

const AlertRecord* AlertsService::get(uint8_t idx) const {
    if (idx >= _count) return nullptr;
    return &_records[idx];
}

const AlertRecord* AlertsService::find(uint16_t alert_id) const {
    int idx = _findIndexById(alert_id);
    return idx >= 0 ? &_records[idx] : nullptr;
}

bool AlertsService::ack(uint16_t alert_id) {
    int idx = _findIndexById(alert_id);
    if (idx < 0) return false;

    // Compact the array: shift later records down by one. List size <= 16
    // so cost is negligible vs maintaining a free-list.
    for (uint8_t i = (uint8_t)idx; i + 1 < _count; i++) {
        _records[i] = _records[i + 1];
    }
    _count--;
    _syncToRTC();
    _emit(EV_ALERT_CLEARED, alert_id);
    return true;
}

void AlertsService::clearAll() {
    // Emit one EV_ALERT_CLEARED per record so subscribers can update
    // (LedService fade-out, UI refresh) on each removal.
    while (_count > 0) {
        const uint16_t id = _records[_count - 1].id;
        _count--;
        _emit(EV_ALERT_CLEARED, id);
    }
    _syncToRTC();
}

void AlertsService::onEvent(const Event& e) {
    switch (e.id) {
        case CMD_ALERT_ACK:
            ack(e.data.alert_cmd.alert_id);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

int AlertsService::_findIndexById(uint16_t alert_id) const {
    if (alert_id == INVALID_ALERT) return -1;
    for (uint8_t i = 0; i < _count; i++) {
        if (_records[i].id == alert_id) return i;
    }
    return -1;
}

int AlertsService::_findIndexByDedup(AlertType type, const char* identifier) const {
    if (!identifier || !identifier[0]) return -1;
    for (uint8_t i = 0; i < _count; i++) {
        if (_records[i].type == type &&
            strncmp(_records[i].identifier, identifier, sizeof(_records[i].identifier)) == 0) {
            return i;
        }
    }
    return -1;
}

void AlertsService::_emit(EventId id, uint16_t alert_id) {
    if (!_bus) return;
    EventPayload p{};
    p.alert.alert_id = alert_id;
    p.alert.state    = (uint8_t)id;   // subscribers can branch by id alone; state is informational
    if (!_bus->post(id, p)) {
        // A dropped RAISED means an alert that IS in the store gets no card,
        // LED, vibe, or screen wake — and re-fires dedup silently forever
        // after. Never let that happen without a trace.
        Serial.printf("[alerts] event %u for alert %u dropped (bus queue full)\n",
                      (unsigned)id, (unsigned)alert_id);
    }
}
