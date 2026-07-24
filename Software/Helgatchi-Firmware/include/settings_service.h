#pragma once
#include "event_bus.h"
#include "settings_keys.h"

class SettingsService : public IEventHandler {
public:
    // Incremented any time the NVS schema layout changes (forces defaults on next boot).
    static constexpr uint16_t SCHEMA_VERSION = 11;  // bumped: SKEY_SCREEN_TIMEOUT_S → SKEY_SLEEP_WHILE_CHARGING (slot reused)

    // Load NVS, apply defaults if schema mismatch, subscribe to commands.
    void begin(EventBus& bus);

    // Read any setting by key. Thread-safe (values are word-aligned atomics on ESP32).
    uint32_t get(SettingsKey key) const;

    // Convenience casts
    bool     getBool(SettingsKey key) const { return get(key) != 0; }
    uint16_t getU16(SettingsKey key)  const { return static_cast<uint16_t>(get(key)); }

    // IEventHandler — CMD_SETTINGS_SET / SAVE / RESET_DEFAULTS, plus EV_TICK_1S
    // to drive the periodic backstop flush.
    void onEvent(const Event& e) override;

    // Persist any pending (dirty) change to NVS now; no-op if nothing is dirty.
    // Call before any power-loss-imminent event (sleep / reboot).
    void flush();

private:
    void _applyDefaults();
    void _applyPerfPreset(PerfMode mode, uint32_t& mask_out);
    bool _set(SettingsKey key, uint32_t value, uint32_t& mask_out);
    void _load();
    void _save();
    void _emitChanged(uint32_t mask);

    uint32_t  _values[SKEY_COUNT] = {};
    EventBus* _bus                = nullptr;
    uint16_t  _change_seq         = 0;     // increments each EV_SETTINGS_CHANGED emission

    // Settings live in RAM; NVS is written only on power-loss-imminent events
    // (sleep/reboot, via PowerManager flush() calls) plus a periodic backstop
    // (SETTINGS_SAVE_BACKSTOP_MS) — never per change. _dirty marks a pending
    // write; _dirty_since_ms starts the backstop clock at the first change.
    bool      _dirty              = false;
    uint32_t  _dirty_since_ms     = 0;
};

extern SettingsService g_settings;
