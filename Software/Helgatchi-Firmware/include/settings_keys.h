#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Supporting enums
// ---------------------------------------------------------------------------

enum PerfMode : uint8_t {
    PERF_PERFORMANCE = 0,  // near-continuous scan, long wake duration
    PERF_BALANCED,         // moderate duty cycle, default timeouts
    PERF_BATTERY_SAVER,    // long intervals, short windows, aggressive sleep
    PERF_DYNAMIC,          // runtime auto-adjustment based on battery level
    PERF_ALWAYS_ON,        // while charging: never sleeps, screen stays on, ~0 gap
                           // between scan windows. On battery: normal duty cycle
                           // (this mode is meant for external power).
    PERF_MODE_COUNT
};

enum ScanMode : uint8_t {
    SCAN_DISABLED = 0,
    SCAN_BLE_ONLY,
    SCAN_WIFI_ONLY,
    SCAN_BLE_AND_WIFI,
    SCAN_MODE_COUNT
};

enum ScreenBrightness : uint8_t {
    SCREEN_BRIGHTNESS_MIN = 0,    // dim but still legible — floor for the user
    SCREEN_BRIGHTNESS_LOW,
    SCREEN_BRIGHTNESS_MEDIUM,
    SCREEN_BRIGHTNESS_HIGH,
    SCREEN_BRIGHTNESS_MAX,
    SCREEN_BRIGHTNESS_COUNT
};

enum LEDBrightness : uint8_t {
    LED_BRIGHTNESS_LOW = 0,
    LED_BRIGHTNESS_MEDIUM,
    LED_BRIGHTNESS_HIGH,
    LED_BRIGHTNESS_MAX,
    LED_BRIGHTNESS_COUNT
};

enum DebugLevel : uint8_t {
    DEBUG_INFORMATIONAL = 0,
    DEBUG_HIGH,
    DEBUG_RENDERING_PERF,
    DEBUG_SCANNING_PERF,
    DEBUG_PERF,             // periodic (1 s) memory + scan-pressure + loop-timing telemetry (human-readable)
    DEBUG_TELEPLOT,         // periodic (1 s) Teleplot ">k:v" stream for live graphing
    DEBUG_LEVEL_COUNT
};

// ---------------------------------------------------------------------------
// Settings keys
//
// [USER]    — shown on Settings screen, user can change
// [DEBUG]   — only shown when debug menu is unlocked
// [DERIVED] — set automatically (by perf mode, build flags, or linked setting)
//             not shown in UI
//
// NOTE: The following debug screen items are ACTIONS, not stored settings.
//       They emit commands — see event_ids.h:
//         "Reset statistics"         → CMD_STATS_RESET
//         "Reset settings"           → CMD_SETTINGS_RESET_DEFAULTS
//         "Shipping mode reset+sleep"→ CMD_POWER_SHIPPING_RESET
// ---------------------------------------------------------------------------

enum SettingsKey : uint8_t {

    // --- Display ---
    SKEY_SCREEN_BRIGHTNESS = 0,     // [USER]    ScreenBrightness
    SKEY_LED_BRIGHTNESS,            // [USER]    LEDBrightness

    // --- Scanning ---
    SKEY_SCAN_MODE,                 // [USER]    ScanMode
    SKEY_SCAN_ACTIVE,               // [USER]    bool — active scan (solicit scan responses) vs passive
    SKEY_PERF_MODE,                 // [USER]    PerfMode

    // --- Alerts ---
    SKEY_ALERT_WAKE_SCREEN,         // [USER]    bool — alert wakes display
    SKEY_ALERT_VIBRATION,           // [USER]    bool
    SKEY_ALERT_LED,                 // [USER]    bool
    SKEY_ALERT_FOCUS,               // [USER]    bool — alert loads the Alerts screen (except while on Settings)

    // --- Power ---
    SKEY_SLEEP_WHILE_USB,           // [USER]    bool — allow sleep when USB attached (no serial)
    SKEY_VSENSE_5V_DIVIDER,         // [USER]    bool — HW has R4 (+5V→VSENSE) populated; changes VBATT math

    // --- Debug (gated by debug menu) ---
    SKEY_DEBUG_SERIAL_ENABLED,      // [DEBUG]   bool — USB serial debug output
    SKEY_DEBUG_LEVEL,               // [DEBUG]   DebugLevel
    SKEY_DEBUG_SLEEP_WITH_SERIAL,   // [DEBUG]   bool — allow sleep even when serial connected

    // Occupies the slot vacated by the removed [DERIVED] SKEY_SCREEN_TIMEOUT_S
    // (which was dead — nothing consumed it). Kept in place rather than appended;
    // SCHEMA_VERSION is bumped so the old value in this slot resets to default
    // instead of being misread as this bool.
    SKEY_SLEEP_WHILE_CHARGING,      // [USER]    bool — allow sleep while charging (default off = inhibit; peer to SLEEP_WHILE_USB / DEBUG_SLEEP_WITH_SERIAL)

    // --- Internal / derived (not shown in UI) ---
    SKEY_INTERACTIVE_TIMEOUT_S,     // [DERIVED] seconds of inactivity before sleep (resets on EV_UI_ACTIVITY)
    SKEY_SLEEP_DURATION_S,          // [DERIVED] seconds the device sleeps between scan cycles
    SKEY_SCAN_DURATION_S,           // [DERIVED] seconds to scan per wake cycle; tuned by perf mode

    SKEY_TUTORIAL_SHOWN,            // [INTERNAL] bool — cleared on first flash / shipping wake

    // Appended here (not in the Scanning section) to keep existing NVS "kNN"
    // indices stable — settings persist one uint32 per enum index, so inserting
    // mid-enum would silently remap every later key on already-flashed devices.
    // Logically a [USER] scanning setting; its UI toggle lives in the Scanning
    // section in EEZ.
    SKEY_IGNORE_RANDOMIZED_MACS,    // [USER]    bool — hide nameless RPA/NRPA BLE devices from the device list (still processed for rules/alerts)

    // Appended (see the NVS-index note above): foxhunt haptics are experimental,
    // so they ship default-OFF behind this toggle. LedService reads it on demand.
    SKEY_HUNT_VIBRATION,            // [USER]    bool — pulse the motor with the foxhunt LED meter (default off)

    SKEY_COUNT,
    SKEY_INVALID = 0xFF
};

// ---------------------------------------------------------------------------
// Change-mask bits — carried in EV_SETTINGS_CHANGED.mask
// Subscribers check only their own bit(s) to avoid unnecessary updates.
// ---------------------------------------------------------------------------

static constexpr uint32_t SMASK_SCAN  = (1u << 0);  // Scanner Service
static constexpr uint32_t SMASK_POWER = (1u << 1);  // Power Manager
static constexpr uint32_t SMASK_MESH  = (1u << 2);  // Device Bridge (reserved, mesh removed for v1)
static constexpr uint32_t SMASK_ALERT = (1u << 3);  // Alert Manager
static constexpr uint32_t SMASK_UI    = (1u << 4);  // UI Controller (brightness, screen timeout)
static constexpr uint32_t SMASK_DEBUG = (1u << 5);  // Logging / debug subsystem
static constexpr uint32_t SMASK_ALL   = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Performance mode presets (applied when SKEY_PERF_MODE is written)
// PERF_DYNAMIC has no preset — the Power Manager adjusts values at runtime.
// ---------------------------------------------------------------------------

struct PerfPreset {
    uint16_t scan_duration_s;        // BLE radio on for this many seconds per cycle
    uint16_t sleep_duration_s;       // BLE radio off / device asleep for this many seconds per cycle
    uint16_t interactive_timeout_s;  // inactivity → sleep when user is interacting
};

// Indexed by PerfMode; PERF_DYNAMIC entry is intentionally zeroed (not used).
// `iact` is the interactive timeout — how long after the last user interaction
// the screen stays lit before deep-sleeping. Last 5 s of this period the
// screen dims as a "going to sleep" warning (handled in PowerManager::tick).
static constexpr PerfPreset PERF_PRESETS[PERF_MODE_COUNT] = {
    // PERFORMANCE   scan  sleep  iact
    {                  7,    15,   20 },
    // BALANCED
    {                  5,    30,   20 },
    // BATTERY_SAVER
    {                  3,    45,   15 },
    // DYNAMIC — managed at runtime, no fixed preset
    {                  0,     0,    0 },
    // ALWAYS_ON — while charging PowerManager overrides these to never-sleep,
    // screen-on, and a ~0 gap between windows (continuous scan). These values
    // are the on-battery fallback duty cycle (this mode targets external power).
    {                 15,    30,   20 },
};

// ---------------------------------------------------------------------------
// Factory defaults
// ---------------------------------------------------------------------------

static constexpr uint8_t  DEFAULT_SCREEN_BRIGHTNESS      = SCREEN_BRIGHTNESS_MEDIUM;
static constexpr uint8_t  DEFAULT_LED_BRIGHTNESS         = LED_BRIGHTNESS_LOW;
static constexpr uint8_t  DEFAULT_SCAN_MODE              = SCAN_BLE_ONLY;
static constexpr uint8_t  DEFAULT_SCAN_ACTIVE            = 0;   // 0 = passive scan (listen only); 1 = active scan (solicit scan responses)
static constexpr uint8_t  DEFAULT_PERF_MODE              = PERF_BALANCED;
static constexpr uint8_t  DEFAULT_ALERT_WAKE_SCREEN      = 1;
static constexpr uint8_t  DEFAULT_ALERT_VIBRATION        = 1;
static constexpr uint8_t  DEFAULT_ALERT_LED              = 1;
static constexpr uint8_t  DEFAULT_ALERT_FOCUS            = 0;
static constexpr uint8_t  DEFAULT_DEBUG_SERIAL           = 0;
static constexpr uint8_t  DEFAULT_DEBUG_LEVEL            = DEBUG_INFORMATIONAL;
static constexpr uint8_t  DEFAULT_SLEEP_WITH_SERIAL      = 0;
static constexpr uint8_t  DEFAULT_SLEEP_WHILE_USB        = 0;   // 0 = inhibit sleep while a USB data host is attached
static constexpr uint8_t  DEFAULT_SLEEP_WHILE_CHARGING   = 0;  // 0 = inhibit sleep while charging
static constexpr uint8_t  DEFAULT_VSENSE_5V_DIVIDER      = 1;   // 0 = R4 cut (VSENSE = VBATT/2, current default)
static constexpr uint8_t  DEFAULT_TUTORIAL_SHOWN         = 0;   // 0 = show on first boot
static constexpr uint8_t  DEFAULT_IGNORE_RANDOMIZED_MACS = 1;   // 1 = hide nameless randomized BLE devices from the list
static constexpr uint8_t  DEFAULT_HUNT_VIBRATION         = 0;   // 0 = foxhunt haptics off (experimental; opt in)
