#include "settings_service.h"
#include <Preferences.h>
#include <Arduino.h>   // millis()

SettingsService g_settings;

// NVS namespace and key format — "k00".."k16", one uint32 per setting.
static constexpr char NVS_NS[]      = "helgatchi";
static constexpr char NVS_VER_KEY[] = "ver";

// Maps each SettingsKey to the SMASK_* bits it affects.
// Must stay in sync with the SettingsKey enum order.
static constexpr uint32_t s_key_mask[SKEY_COUNT] = {
    SMASK_UI,                   // SKEY_SCREEN_BRIGHTNESS
    SMASK_UI,                   // SKEY_LED_BRIGHTNESS
    SMASK_SCAN,                 // SKEY_SCAN_MODE
    SMASK_SCAN,                 // SKEY_SCAN_ACTIVE
    SMASK_SCAN | SMASK_POWER,   // SKEY_PERF_MODE
    SMASK_ALERT,                // SKEY_ALERT_WAKE_SCREEN
    SMASK_ALERT,                // SKEY_ALERT_VIBRATION
    SMASK_ALERT,                // SKEY_ALERT_LED
    SMASK_ALERT,                // SKEY_ALERT_FOCUS
    SMASK_POWER,                // SKEY_SLEEP_WHILE_USB
    SMASK_POWER,                // SKEY_VSENSE_5V_DIVIDER
    SMASK_DEBUG,                // SKEY_DEBUG_SERIAL_ENABLED
    SMASK_DEBUG,                // SKEY_DEBUG_LEVEL
    SMASK_DEBUG | SMASK_POWER,  // SKEY_DEBUG_SLEEP_WITH_SERIAL
    SMASK_POWER,                // SKEY_SLEEP_WHILE_CHARGING
    SMASK_POWER,                // SKEY_INTERACTIVE_TIMEOUT_S
    SMASK_POWER,                // SKEY_SLEEP_DURATION_S
    SMASK_POWER,                // SKEY_SCAN_DURATION_S — PowerManager owns the scan window; ScanEngine never reads it
    0,                          // SKEY_TUTORIAL_SHOWN — no subsystem reacts
    SMASK_SCAN,                 // SKEY_IGNORE_RANDOMIZED_MACS — read by ScanService::noiseSuppressed
                                //   (seen-map insert + device-list render)
    0,                          // SKEY_HUNT_VIBRATION — LedService reads it on demand each hunt frame
    SMASK_DEBUG,                // SKEY_DEBUG_ENABLED — non-zero so EV_SETTINGS_CHANGED fires and the settings screen re-applies row visibility
};

// ---------------------------------------------------------------------------

// Settings live in RAM and are flushed to NVS on power-loss-imminent events
// (sleep / shipping / reboot — PowerManager calls flush()). This is a periodic
// backstop: once a change is pending, commit it at most this often, so an
// ungraceful loss (crash, USB yank with no battery, battery death before the
// low-battery safe-shutdown) loses at most this much. Keeps per-change flash
// writes — and their phase_bus spike — out of normal interactive use.
static constexpr uint32_t SETTINGS_SAVE_BACKSTOP_MS = 60000;   // 60 s

void SettingsService::begin(EventBus& bus) {
    _bus = &bus;
    _applyDefaults();
    _load();

    bus.subscribe(CMD_SETTINGS_SET,            this);
    bus.subscribe(CMD_SETTINGS_SAVE,           this);
    bus.subscribe(CMD_SETTINGS_RESET_DEFAULTS, this);
    bus.subscribe(EV_TICK_1S,                  this);   // drives the deferred NVS flush
}

uint32_t SettingsService::get(SettingsKey key) const {
    if (key >= SKEY_COUNT) return 0;
    return _values[key];
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void SettingsService::onEvent(const Event& e) {
    switch (e.id) {
        case CMD_SETTINGS_SET: {
            auto key = static_cast<SettingsKey>(e.data.settings_set.key);
            uint32_t mask = 0;
            // Change stays in RAM (even mask-0 keys like SKEY_TUTORIAL_SHOWN);
            // it's persisted on sleep/reboot or the periodic backstop, never
            // per change. Only emit EV_SETTINGS_CHANGED when a subsystem reacts.
            if (_set(key, e.data.settings_set.value, mask)) {
                // Debug toggle OFF also resets the [DEBUG] keys to defaults —
                // no invisible noisy debug level left running behind hidden
                // rows. Service-level so the UI switch and serial `setting
                // set` behave identically (cascade precedent: _applyPerfPreset).
                if (key == SKEY_DEBUG_ENABLED && e.data.settings_set.value == 0) {
                    _set(SKEY_DEBUG_SERIAL_ENABLED, DEFAULT_DEBUG_SERIAL, mask);
                    _set(SKEY_DEBUG_LEVEL,          DEFAULT_DEBUG_LEVEL,  mask);
                }
                if (!_dirty) {                 // start the backstop clock at the first change
                    _dirty          = true;
                    _dirty_since_ms = millis();
                }
                if (mask) _emitChanged(mask);
            }
            break;
        }
        case CMD_SETTINGS_SAVE:
            flush();                    // explicit "save now" (serial `settings save`)
            break;

        case CMD_SETTINGS_RESET_DEFAULTS:
            _applyDefaults();
            _dirty = true;
            flush();                    // deliberate reset — persist immediately
            _emitChanged(SMASK_ALL);
            break;

        case EV_TICK_1S:
            // Periodic backstop only — a pending change is normally persisted at
            // the next sleep/reboot; this bounds worst-case loss to BACKSTOP_MS
            // when the device never gets a graceful power-down (e.g. sleep-
            // inhibited on USB) before an ungraceful one.
            if (_dirty && (millis() - _dirty_since_ms) >= SETTINGS_SAVE_BACKSTOP_MS) {
                flush();
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void SettingsService::_applyDefaults() {
    _values[SKEY_SCREEN_BRIGHTNESS]       = DEFAULT_SCREEN_BRIGHTNESS;
    _values[SKEY_LED_BRIGHTNESS]          = DEFAULT_LED_BRIGHTNESS;
    _values[SKEY_SCAN_MODE]               = DEFAULT_SCAN_MODE;
    _values[SKEY_SCAN_ACTIVE]             = DEFAULT_SCAN_ACTIVE;
    _values[SKEY_PERF_MODE]               = DEFAULT_PERF_MODE;
    _values[SKEY_ALERT_WAKE_SCREEN]       = DEFAULT_ALERT_WAKE_SCREEN;
    _values[SKEY_ALERT_VIBRATION]         = DEFAULT_ALERT_VIBRATION;
    _values[SKEY_ALERT_LED]               = DEFAULT_ALERT_LED;
    _values[SKEY_ALERT_FOCUS]             = DEFAULT_ALERT_FOCUS;
    _values[SKEY_SLEEP_WHILE_USB]         = DEFAULT_SLEEP_WHILE_USB;
    _values[SKEY_SLEEP_WHILE_CHARGING]    = DEFAULT_SLEEP_WHILE_CHARGING;
    _values[SKEY_VSENSE_5V_DIVIDER]       = DEFAULT_VSENSE_5V_DIVIDER;
    _values[SKEY_DEBUG_SERIAL_ENABLED]    = DEFAULT_DEBUG_SERIAL;
    _values[SKEY_DEBUG_LEVEL]             = DEFAULT_DEBUG_LEVEL;
    _values[SKEY_DEBUG_SLEEP_WITH_SERIAL] = DEFAULT_SLEEP_WITH_SERIAL;

    _values[SKEY_TUTORIAL_SHOWN]          = DEFAULT_TUTORIAL_SHOWN;
    _values[SKEY_IGNORE_RANDOMIZED_MACS]  = DEFAULT_IGNORE_RANDOMIZED_MACS;
    _values[SKEY_HUNT_VIBRATION]          = DEFAULT_HUNT_VIBRATION;
    _values[SKEY_DEBUG_ENABLED]           = DEFAULT_DEBUG_ENABLED;

    uint32_t dummy = 0;
    _applyPerfPreset(static_cast<PerfMode>(DEFAULT_PERF_MODE), dummy);
}

void SettingsService::_applyPerfPreset(PerfMode mode, uint32_t& mask_out) {
    if (mode == PERF_DYNAMIC) return; // dynamic values are managed at runtime by Power Manager

    const PerfPreset& p = PERF_PRESETS[mode];
    _values[SKEY_SCAN_DURATION_S]        = p.scan_duration_s;
    _values[SKEY_SLEEP_DURATION_S]       = p.sleep_duration_s;
    _values[SKEY_INTERACTIVE_TIMEOUT_S]  = p.interactive_timeout_s;

    mask_out |= SMASK_SCAN | SMASK_POWER;
}

bool SettingsService::_set(SettingsKey key, uint32_t value, uint32_t& mask_out) {
    if (key >= SKEY_COUNT) return false;
    if (_values[key] == value) return false;

    _values[key] = value;
    mask_out |= s_key_mask[key];

    if (key == SKEY_PERF_MODE) {
        _applyPerfPreset(static_cast<PerfMode>(value), mask_out);
    }
    return true;
}

void SettingsService::_load() {
    Preferences prefs;
    prefs.begin(NVS_NS, /*readOnly=*/true);

    uint16_t stored_ver = prefs.getUShort(NVS_VER_KEY, 0);
    if (stored_ver != SCHEMA_VERSION) {
        prefs.end();
        // Schema mismatch — defaults already applied in begin(); persist them now.
        _save();
        return;
    }

    char key[5];
    for (uint8_t i = 0; i < SKEY_COUNT; i++) {
        snprintf(key, sizeof(key), "k%02u", i);
        _values[i] = prefs.getUInt(key, _values[i]);
    }
    prefs.end();
}

void SettingsService::_save() {
    Preferences prefs;
    prefs.begin(NVS_NS, /*readOnly=*/false);
    prefs.putUShort(NVS_VER_KEY, SCHEMA_VERSION);

    char key[5];
    for (uint8_t i = 0; i < SKEY_COUNT; i++) {
        snprintf(key, sizeof(key), "k%02u", i);
        prefs.putUInt(key, _values[i]);
    }
    prefs.end();
}

void SettingsService::flush() {
    if (!_dirty) return;
    _save();
    _dirty = false;
}

void SettingsService::_emitChanged(uint32_t mask) {
    EventPayload p{};
    p.settings.mask    = mask;
    p.settings.version = ++_change_seq;
    _bus->post(EV_SETTINGS_CHANGED, p);
}
