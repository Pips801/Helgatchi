#pragma once
#include "event_bus.h"

// ---------------------------------------------------------------------------
// Power states — carried in EV_POWER_STATE_CHANGED.data.power.state
// ---------------------------------------------------------------------------
enum PowerState : uint8_t {
    POWER_AWAKE    = 0,
    POWER_SLEEPING = 1,
};

// ---------------------------------------------------------------------------
// Battery pct sentinels (values above 100 encode special states)
// ---------------------------------------------------------------------------
static constexpr uint8_t BATT_PCT_CHARGING = 200;
static constexpr uint8_t BATT_PCT_CHARGED  = 201;
static constexpr uint8_t BATT_PCT_MISSING  = 202;

// ---------------------------------------------------------------------------
// VSENSE divider variants (SKEY_VSENSE_5V_DIVIDER)
//
// The battery-sense pin reads a resistor-divided node. Two board builds:
//
//   • R4 cut (default): R2(100k) VBATT→node, R3(100k) node→GND. Plain 2:1
//     divider → VSENSE = VBATT/2, so VBATT = 2·VSENSE in every state. The +5V
//     charger rail is invisible here, so charge state is inferred from USB data
//     presence (HAL::usbAttached()) alone — a dumb charger can't be detected.
//
//   • R4 populated (SKEY_VSENSE_5V_DIVIDER=1): R2/R3/R4 (all 100k) meet at the
//     node, R4 tied to the +5V charger rail → VSENSE = (VBATT + V5)/3, with V5
//     the rail (≈5 V charging, 0 V unplugged). So the READING ITSELF signals
//     charging (the rail lifts the node well clear of the battery-only range) —
//     no USB-data dependency, dumb chargers included.
//
//     Absolute scale is CALIBRATED from a known-full pack (≈4.2 V), which reads
//     1590 mV unplugged and 2972 mV on USB. Two facts fall out, both in raw
//     ADC-mV so they already absorb the ESP32-S3 ADC's ~13 % over-read on this
//     33 kΩ node and the real ~4.15 V rail (not a nominal 5.0 V):
//        • charging adds a FIXED  PM_R4_CHARGE_OFFSET_MV (= 2972−1590 = V5/3)
//        • after removing it, VBATT = VSENSE · PM_R4_VBATT_NUM/PM_R4_VBATT_DEN
//     so full → ~4198 mV / 100 % whether plugged or not.
//
//     Presence/charge bands in raw VSENSE mV:
//        battery only (discharge) : ≤ ~1620   (drops toward 0 as it depletes)
//        no pack, on USB          : ~1382     ((0+V5)/3, VBATT rail pulled low)
//        charging (pack present)  : ~2500 … 2972
//     PM_R4_CHARGING_MV splits "charging" from everything below it. On USB a
//     present pack is ALWAYS in the charging band, so a reading below the
//     threshold there means the pack is missing/faulty (BATT_PCT_MISSING).
//
// The discharge % curve is consulted only while discharging (and while charging
// to decide "full"), fed the VBATT/2 value == batt_mv/2 for both builds.
//
// LiPo discharge is non-linear — voltage stays high through most of the
// useful capacity, then drops sharply near depletion. A naive linear map
// across [3.0, 4.2]V both undersells fresh-charged packs (they never quite
// hit 4.20V at rest under load) and overstates remaining runtime in the
// middle of the curve.
//
// We use a piecewise-linear LUT instead, with deliberately conservative
// anchors:
//   100% at 4.15V  — settled-fresh resting voltage; avoids "less than full
//                     bar" when the icon is checked seconds after unplug.
//     0% at 3.40V  — well above the 3.00V protection-cutoff floor; leaves
//                     headroom for radio/LED load sag and protects cell life.
// VBATT readings outside [3.40, 4.15]V are clamped to 0/100.
//
// Points below approximate a typical 1S LiPo discharge curve, expressed in
// vsense-mV (= VBATT / 2). Add/remove points to tune; pmBattPctFromVsenseMv
// linearly interpolates between consecutive entries (must be sorted high→low
// on vsense_mv).
// ---------------------------------------------------------------------------

// R4-populated calibration (raw VSENSE mV / integer ratio). See block above.
static constexpr uint16_t PM_R4_CHARGE_OFFSET_MV = 1382;  // +5V rail's fixed lift (= V5/3)
static constexpr uint16_t PM_R4_VBATT_NUM        = 264;   // VBATT = VSENSE·2.64 (full 1590→4198)
static constexpr uint16_t PM_R4_VBATT_DEN        = 100;

// Charge / no-battery threshold, raw VSENSE mV: above → charging; on USB and
// below → pack missing/faulty (a present pack on USB sits in the charging band).
static constexpr uint16_t PM_R4_CHARGING_MV = 2200;
struct BattCurvePoint { uint16_t vsense_mv; uint8_t pct; };
static constexpr BattCurvePoint PM_BATT_CURVE[] = {
    {2075, 100},  // 4.15 V
    {2025,  90},  // 4.05 V
    {1975,  75},  // 3.95 V
    {1925,  55},  // 3.85 V
    {1900,  40},  // 3.80 V
    {1875,  25},  // 3.75 V
    {1825,  10},  // 3.65 V
    {1700,   0},  // 3.40 V
};
static constexpr unsigned PM_BATT_CURVE_N    = sizeof(PM_BATT_CURVE) / sizeof(PM_BATT_CURVE[0]);
static constexpr uint16_t PM_VSENSE_FULL_MV  = PM_BATT_CURVE[0].vsense_mv;
static constexpr uint16_t PM_VSENSE_DEAD_MV  = PM_BATT_CURVE[PM_BATT_CURVE_N - 1].vsense_mv;

inline uint8_t pmBattPctFromVsenseMv(uint16_t vsense_mv) {
    if (vsense_mv >= PM_VSENSE_FULL_MV) return 100;
    if (vsense_mv <= PM_VSENSE_DEAD_MV) return 0;
    for (unsigned i = 0; i + 1 < PM_BATT_CURVE_N; ++i) {
        const BattCurvePoint& hi = PM_BATT_CURVE[i];
        const BattCurvePoint& lo = PM_BATT_CURVE[i + 1];
        if (vsense_mv <= hi.vsense_mv && vsense_mv >= lo.vsense_mv) {
            int32_t span_mv  = (int32_t)hi.vsense_mv - lo.vsense_mv;
            int32_t span_pct = (int32_t)hi.pct      - lo.pct;
            int32_t off_mv   = (int32_t)vsense_mv   - lo.vsense_mv;
            return (uint8_t)(lo.pct + (off_mv * span_pct) / span_mv);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------

class PowerManager : public IEventHandler {
public:
    // Call FIRST in setup(), before any other init. On a button (EXT1) wake
    // from deep sleep — regular or shipping — this requires the user to hold
    // CENTER (both PIN_BTN_1 and PIN_BTN_2 LOW) for the hold window (shorter
    // for regular sleep, longer for shipping — see the *_WAKE_HOLD_MS consts).
    //   • Held for full duration → boot continues (shipping flag cleared)
    //   • Released early / not center → re-enters the same sleep, never returns
    // Timer wake (scan cycle), cold boot, and soft reset → returns immediately.
    static void checkWakeHoldOrResleep();

    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Turn the display off without entering deep sleep. Cleared by a button
    // press or an alert raised with SKEY_ALERT_WAKE_SCREEN enabled. Serial
    // input does not clear it.
    void sleepScreen();

    // Force the display on, clearing the sleepScreen() override — the same
    // effect as a button press. Needed by serially-triggered power actions:
    // with the display off, rendering AND lv_timers are suspended, so the
    // Power Action screen's countdown would never fire on a dark screen.
    void wakeScreen();

    // Deep-sleep if conditions permit, otherwise just turn the screen off.
    // Wraps the inhibit check so callers don't have to duplicate it.
    void requestSleepOrScreenOff();

    // Last EV_BATTERY_UPDATED values posted to the bus (for diagnostics).
    // pct may be a BATT_PCT_* sentinel; mv is always the literal vbatt mv last
    // sampled. 0xFF in pct = no sample taken yet.
    uint16_t lastBatteryMv()  const { return _last_batt_mv;  }
    uint8_t  lastBatteryPct() const { return _last_batt_pct; }

    // Seconds until the next scan window opens (diagnostics). Returns 0 while a
    // scan window is currently open, and 0xFFFF when scanning is disabled
    // (SKEY_SCAN_MODE == 0). Mirrors the between-window timing in tick().
    uint16_t secondsUntilNextScan() const;

    // Per-radio scan duration (resolved: 0→default fallback applied, tracks any
    // future PERF_DYNAMIC adjustment). ScanEngine reads this so its intra-window
    // BLE→WiFi phase boundary derives from the same value as our total window.
    uint16_t scanDurationS() const { return _scan_duration_s; }

private:
    // Number of enabled scan radios (SKEY_SCAN_MODE bit 0 = BLE, bit 1 = WiFi).
    // The scan window runs each enabled radio back-to-back for scanDurationS(),
    // so the total window is scanDurationS() × this (min 1 so an all-disabled
    // cycle still keeps the same idle cadence).
    uint8_t _enabledRadioCount() const;
    uint32_t _scanWindowMs() const;

    enum class DisplayState : uint8_t { OFF, ON, DIM };

    void _syncSettings();
    void _sampleBattery();
    void _enterSleep();
    void _wipeUserState();           // shared factory wipe across all services (applied + persisted on return)
    void _factoryResetAndShip();     // wipe, then shipping sleep — assembly line (`power shipping`)
    void _factoryResetAndReboot();   // wipe, then reboot — user-reachable Reset device action
    void _enterShippingSleep();
    void _enterPowerDown();        // button-only deep sleep, keeps the tutorial flag
    void _enterOffSleep(bool reset_tutorial);  // shared no-timer deep-sleep path
    void _reboot();                // peripheral teardown + ESP.restart()
    void _postCountdown(uint32_t now);
    uint32_t _calcRemainingS(uint32_t now) const;
    bool _isInhibited();           // non-const: maintains hysteresis state
    void _setDisplay(DisplayState s);

    EventBus* _bus = nullptr;
    DisplayState _disp_state = DisplayState::OFF;
    uint32_t _last_inhibit_seen_ms = 0;  // hysteresis timestamp for _isInhibited

    // Timing
    uint32_t _wake_ms          = 0;
    uint32_t _last_activity_ms = 0;
    uint32_t _last_batt_ms     = 0;
    uint32_t _last_tick_ms     = 0;
    uint32_t _stop_ms          = 0;   // millis() when last CMD_SCAN_STOP was posted

    // Cached settings
    uint16_t _scan_duration_s          = 5;
    uint16_t _sleep_duration_s         = 30;
    uint16_t _interactive_timeout_s    = 30;
    bool     _sleep_w_serial           = false;  // SKEY_DEBUG_SLEEP_WITH_SERIAL — true = allow sleep with serial open
    bool     _sleep_while_usb          = false;  // SKEY_SLEEP_WHILE_USB         — true = allow sleep with a USB data host attached (usbAttached)
    bool     _sleep_while_charging     = false;  // SKEY_SLEEP_WHILE_CHARGING    — true = allow sleep while charging (_is_charging)
    bool     _always_on                = false;  // SKEY_PERF_MODE == PERF_ALWAYS_ON (cached; drives the never-sleep/continuous-scan behavior while charging)
    bool     _vsense_5v_divider        = false;  // SKEY_VSENSE_5V_DIVIDER       — true = R4 (+5V→VSENSE) populated

    // State
    bool _user_active          = false;  // true once EV_UI_ACTIVITY received this cycle
    bool _scan_stop_posted     = false;
    bool _scan_complete_posted = false;  // EV_SCAN_COMPLETE fired once per window after drain
    bool _is_charging          = false;  // true when charging detected (R4: ADC-sensed 5V; else USB data)
    bool _screen_off_override  = false;  // sleepScreen() set; cleared by buttons or wake-screen alerts
    bool _last_usb_seen        = false;  // HAL::usbAttached() edge tracker (forces an immediate resample)
    bool _hunting              = false;  // foxhunt lock-on active (CMD_SCAN_LOCKON_START..STOP)

    // Battery EMA (smooths single-sample noise / transient load sag).
    // Only applied while discharging — sentinel values pass through unsmoothed,
    // and the EMA is reset on the charging→discharging transition so the first
    // post-unplug sample isn't biased toward the inflated charging voltage.
    bool    _have_smoothed_pct = false;
    uint8_t _smoothed_pct      = 0;

    // Last posted battery values (mirrored for the `battery` serial command).
    uint16_t _last_batt_mv  = 0;
    uint8_t  _last_batt_pct = 0xFF;
};

extern PowerManager g_power;
