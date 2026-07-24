#pragma once
#include "event_bus.h"
#include "led_service.h"
#include "vibe_service.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Alerts service
//
// Central store of active alerts. Decoupled from the rules engine — anything
// can raise an alert (rules, serial console, mesh bridge later, etc). Side
// effects (vibe / LED / screen wake) are driven by VibeService, LedService,
// and PowerManager subscribing to EV_ALERT_RAISED; this service emits that
// event after registering the alert. Each subscriber gates itself on its own
// SKEY_ALERT_* setting and queries g_alerts for the specific patterns to
// play.
//
// Lifecycle: records persist until ack()'d (or `alert clear` over serial).
// Future enhancement candidates — auto-dismissal after N seconds, silencing
// repeated alerts — are deferred until product feedback says they're needed.
//
// Dedup: re-raising an alert with the same (type, identifier) coalesces into
// the existing record — updates last_seen_ms, bumps seen_count, emits
// EV_ALERT_UPDATED (no side effects re-fire). Empty identifier disables
// dedup, so serial test alerts can pile up freely.
// ---------------------------------------------------------------------------

enum AlertType : uint8_t {
    ALERT_BLE = 0,           // bluetooth icon — BLE rule matches
    ALERT_WIFI,              // wifi icon — WiFi rule matches
    ALERT_SYSTEM,            // bell icon — generic system notification (default)
    ALERT_BATTERY_LOW,       // empty-battery icon — PowerManager fires when battery is low
    ALERT_TYPE_COUNT
};

struct AlertRecord {
    uint16_t        id;                 // 0 = invalid; service assigns
    AlertType       type;
    HapticPatternId vibe;               // pattern to play on first raise
    LedPatternId    led;
    int8_t          rssi;               // INT8_MIN = unknown / not applicable
    uint32_t        first_seen_ms;      // millis() at first raise
    uint32_t        last_seen_ms;       // millis() at most recent re-raise
    uint16_t        seen_count;         // 1 = single occurrence; >1 = deduped
    char            title[32];          // truncated if longer (always null-terminated)
    char            identifier[72];     // dedup key — must fit RulesService's
                                        // "<name[56]>:<12 hex MAC>" (69 chars);
                                        // truncation here collapses distinct
                                        // devices into one alert
};

class AlertsService : public IEventHandler {
public:
    static constexpr uint8_t  MAX_ALERTS    = 16;
    static constexpr uint16_t INVALID_ALERT = 0;

    void begin(EventBus& bus);

    // Register an alert. Returns the alert_id (existing if deduped to an
    // active record, new otherwise) or INVALID_ALERT if capacity is full
    // and there's no dedup match (caller's call is dropped, existing
    // unacked alerts are preserved).
    //
    // title       — required; truncated to fit AlertRecord::title.
    // identifier  — used for dedup with `type`. Empty/null = no dedup.
    // rssi        — INT8_MIN if unknown.
    uint16_t raise(const char* title,
                   AlertType        type,
                   HapticPatternId  vibe,
                   LedPatternId     led,
                   const char*      identifier = nullptr,
                   int8_t           rssi       = INT8_MIN);

    // Read-only access for UI / subscribers. Indices are NOT stable — they
    // can shift when alerts are ack'd. Use find() with alert_id for stable
    // lookups.
    uint8_t            count() const { return _count; }

    // Lifetime count of distinct alerts raised since boot (diagnostics).
    // Deduped re-raises don't bump this; _next_id starts at 1.
    uint16_t           raisedCount() const { return (uint16_t)(_next_id - 1); }
    const AlertRecord* get(uint8_t idx) const;
    const AlertRecord* find(uint16_t alert_id) const;

    // Dismiss an alert. Returns true if a record was removed.
    bool ack(uint16_t alert_id);
    void clearAll();

    // IEventHandler — handles CMD_ALERT_ACK
    void onEvent(const Event& e) override;

private:
    int _findIndexById(uint16_t alert_id) const;
    int _findIndexByDedup(AlertType type, const char* identifier) const;
    void _emit(EventId id, uint16_t alert_id);
    void _syncToRTC();   // mirror state into RTC slow memory after each mutation

    AlertRecord _records[MAX_ALERTS] = {};
    uint8_t     _count               = 0;
    uint16_t    _next_id             = 1;   // 0 reserved as INVALID_ALERT
    EventBus*   _bus                 = nullptr;
};

extern AlertsService g_alerts;
