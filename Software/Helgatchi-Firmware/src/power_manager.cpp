#include "power_manager.h"
#include "scan_power_policy.h"
#include "settings_service.h"
#include "hal.h"
#include "vibe_service.h"
#include "ui_controller.h"
#include "toast_service.h"   // always-on state changes on a plug/unplug edge
#include "log_service.h"
#include "scan_service.h"
#include "scan_engine.h"
#include "rules_service.h"
#include "alerts_service.h"
#include "party_service.h"
#include "admin_service.h"
#include "event_payload.h"
#include "overview_screen.h"
#include "led_service.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_sleep.h>

PowerManager g_power;

static constexpr uint32_t BATTERY_SAMPLE_INTERVAL_MS = 30000;
static constexpr uint32_t TICK_1S_INTERVAL_MS        =  1000;
static constexpr uint32_t R4_EDGE_POLL_MS            =   250;  // charge-edge detector cadence

// ---------------------------------------------------------------------------
// Deep-sleep wake handshake
//
// The button matrix is two RTC-readable-but-only-one-RTC-wake-capable pins:
//   GPIO6  (PIN_BTN_1) — LOW when LEFT or CENTER pressed. RTC-capable → EXT1.
//   GPIO43 (PIN_BTN_2) — LOW when RIGHT or CENTER pressed. NOT RTC-capable.
// So the only viable deep-sleep wake trigger is GPIO6 LOW (left or center).
//
// Deep sleep GPIO wake can't natively require a hold or a specific button, so
// we do it in software at boot: EXT1 wakes the chip on GPIO6 LOW, then
// checkWakeHoldOrResleep() (called FIRST in setup) demands the user hold
// CENTER — both rails LOW — for WAKE_HOLD_MS. A left-only press, a right-only
// press (can't even wake), or a brief accidental bump falls short and the
// device re-enters the exact sleep it came from without spinning anything up.
// This makes pocket carry reliable: nothing but a deliberate center hold wakes.
//
// Two sleep flavors re-armed on a failed hold:
//   • shipping (_shipping_pending) — EXT1 only, never auto-wakes.
//   • regular  — EXT1 + the scan-cycle timer (_deep_sleep_timer_us) when a
//                radio is enabled, so the autonomous scan cadence resumes.
//                Zero-radio mode uses button-only sleep.
//
// TIMER wakes (silent scan cycle) and cold boot / soft reset skip the check
// entirely — no button was involved.
//
// RTC memory survives deep sleep but is cleared on full power loss, so a
// battery pull during sleep cold-boots normally on next power-up.
// ---------------------------------------------------------------------------

RTC_DATA_ATTR static bool     _shipping_pending    = false;
RTC_DATA_ATTR static uint64_t _deep_sleep_timer_us = 0;   // scan-cycle timer, re-armed on failed hold

// Center-hold duration required to wake. Shipping demands a longer, more
// deliberate hold than a normal sleep wake since it's the "unbox / take out
// of storage" gesture and should never trigger by accident.
static constexpr uint32_t SLEEP_WAKE_HOLD_MS    = 1000;
static constexpr uint32_t SHIPPING_WAKE_HOLD_MS = 2200;
static constexpr uint32_t WAKE_POLL_MS          =   10;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void PowerManager::checkWakeHoldOrResleep() {
    // Only button (EXT1) wakes need the hold gate. Timer wake (scan cycle),
    // cold boot, and soft reset proceed straight to normal boot.
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return;

    // A button pulled GPIO6 low. Demand CENTER (both rails LOW) held for the
    // full duration before continuing.
    pinMode(PIN_BTN_1, INPUT_PULLUP);
    pinMode(PIN_BTN_2, INPUT_PULLUP);
    delayMicroseconds(200);  // let pullups settle

    const uint32_t hold_target = _shipping_pending ? SHIPPING_WAKE_HOLD_MS
                                                    : SLEEP_WAKE_HOLD_MS;
    bool held_ok = true;
    uint32_t held = 0;
    while (held < hold_target) {
        if (digitalRead(PIN_BTN_1) != LOW || digitalRead(PIN_BTN_2) != LOW) {
            held_ok = false;  // not center, or released early
            break;
        }
        delay(WAKE_POLL_MS);
        held += WAKE_POLL_MS;
    }

    if (held_ok) {
        // Deliberate center hold — wake for real. Shipping exits shipping mode.
        _shipping_pending = false;
        return;
    }

    // Hold not satisfied. Wait for full release so EXT1 (wake-on-LOW) doesn't
    // immediately re-fire on a still-held left/right button, then re-enter the
    // sleep we came from.
    while (digitalRead(PIN_BTN_1) == LOW || digitalRead(PIN_BTN_2) == LOW) {
        delay(WAKE_POLL_MS);
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_1, ESP_EXT1_WAKEUP_ANY_LOW);
    if (!_shipping_pending && _deep_sleep_timer_us > 0) {
        esp_sleep_enable_timer_wakeup(_deep_sleep_timer_us);  // resume scan cadence
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_deep_sleep_start();
    // Does not return.
}

void PowerManager::begin(EventBus& bus) {
    _bus = &bus;
    _syncSettings();

    // Restore the last learned charger rail: a reboot while still plugged in
    // (every firmware flash) has no discharge→charge edge to learn from, and
    // the fallback constant can sit ~300 mV off the real rail — which lands
    // 1:1 on the reported cell voltage. NVS survives firmware-only flashes.
    {
        Preferences prefs;
        if (prefs.begin("power", /*readOnly=*/true)) {
            uint16_t rail = prefs.getUShort("rail", 0);
            prefs.end();
            if (rail >= PM_R4_CHG_RAIL_MIN_MV && rail <= PM_R4_CHG_RAIL_MAX_MV)
                _chg_rail_mv = rail;
        }
    }

    bus.subscribe(EV_SETTINGS_CHANGED,      this);
    bus.subscribe(EV_UI_ACTIVITY,           this);
    bus.subscribe(EV_ALERT_RAISED,          this);
    bus.subscribe(CMD_POWER_SLEEP,          this);
    bus.subscribe(CMD_POWER_SHIPPING_RESET, this);
    bus.subscribe(CMD_POWER_FACTORY_RESET,  this);
    bus.subscribe(CMD_POWER_SCREEN_OFF,     this);
    bus.subscribe(CMD_POWER_REBOOT,         this);
    bus.subscribe(CMD_POWER_DOWN,           this);
    bus.subscribe(CMD_SCAN_LOCKON_START,    this);
    bus.subscribe(CMD_SCAN_LOCKON_STOP,     this);
    bus.subscribe(EV_BTN_LEFT,              this);
    bus.subscribe(EV_BTN_RIGHT,             this);
    bus.subscribe(EV_BTN_CENTER_SHORT,      this);
    bus.subscribe(EV_BTN_CENTER_LONG,       this);
    bus.subscribe(EV_BTN_WAKE,              this);

    _wake_ms          = millis();
    _last_batt_ms     = _wake_ms;
    _last_tick_ms     = _wake_ms;
    _user_active      = false;
    _scan_stop_posted = false;
    _disp_state       = DisplayState::OFF;
    _last_usb_seen    = g_hal.usbAttached();

    // Decide what the display does based on what woke us:
    //   • TIMER     → autonomous scan cycle, screen stays off
    //   • EXT1      → user pressed a button, light up + interactive timeout
    //   • UNDEFINED → cold boot / soft reset, treat like a button wake so the
    //                 splash + status screens are visible after flashing/reboot
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        // Silent scan wake — leave display off, run the scan-then-sleep cycle.
        _setDisplay(DisplayState::OFF);
    } else {
        _user_active      = true;
        _last_activity_ms = _wake_ms;
        _setDisplay(DisplayState::ON);
    }

    EventPayload p{};
    p.power.state = POWER_AWAKE;
    _bus->post(EV_POWER_STATE_CHANGED, p);

    // Don't scan until the tutorial is completed. An unboxed unit just waits for
    // interaction, then shuts fully off (button-only wake) on the interactive
    // timeout — see the tutorial gate in tick(). No CMD_SCAN_START here means no
    // scan windows and, crucially, no scan-cadence timer wake to duty-cycle on.
    if (g_settings.getBool(SKEY_TUTORIAL_SHOWN)) {
        _bus->post(CMD_SCAN_START);
        _scanning_started = true;
    }

    _sampleBattery();
}

// ---------------------------------------------------------------------------
// Main loop tick
// ---------------------------------------------------------------------------

void PowerManager::tick() {
    uint32_t now = millis();

    // R4 charge-edge detector (250 ms — the status-bar prefix debounces
    // step-downs by 500 ms, so state must flip well inside that window).
    // Two triggers:
    //   • threshold crossing — plug-in; the flip to CHARGING additionally
    //     requires a non-falling node, so the slow post-unplug decay hovering
    //     above the threshold can't flip state back to charging.
    //   • unplug plunge — a ≥PM_R4_UNPLUG_DROP_MV fall in one poll while
    //     charging. A charger-driven node never falls that fast; this sees
    //     cable-out ~1–2 s before the decay crosses the threshold.
    // The flip posts immediately with the carried cell voltage; the deferred
    // settled sample (1 s loop below) re-reads the true voltage and state.
    if (_vsense_5v_divider && now - _last_fast_ms >= R4_EDGE_POLL_MS) {
        _last_fast_ms = now;
        uint16_t mv = g_hal.readVsenseMv();

        bool to_charging  = !_is_charging && mv > PM_R4_CHARGING_MV &&
                            (!_have_fast_mv ||
                             (uint16_t)(mv + PM_R4_SETTLE_DELTA_MV) >= _fast_prev_mv);
        bool to_discharge = _is_charging &&
                            (mv <= PM_R4_CHARGING_MV ||
                             (_have_fast_mv && _fast_prev_mv > mv &&
                              (uint16_t)(_fast_prev_mv - mv) >= PM_R4_UNPLUG_DROP_MV));
        if (to_charging || to_discharge) {
            _settle_pending = true;
            _settle_polls   = 0;
            _last_batt_ms   = now;
            _flipChargeState(to_charging);
        }
        _fast_prev_mv = mv;
        _have_fast_mv = true;
    }

    // 1-second heartbeat.
    if (now - _last_tick_ms >= TICK_1S_INTERVAL_MS) {
        _last_tick_ms += TICK_1S_INTERVAL_MS;
        _bus->post(EV_TICK_1S);
        _postCountdown(now);

        // Dim countdown: when ≤5 s remain in the interactive timeout, drop the
        // screen to MIN brightness as a "going to sleep" warning. Skipped when
        // sleep is inhibited (USB / serial / charging) since the device won't
        // actually sleep, so dimming would be misleading. Always-On while
        // charging forces the panel ON even after a timer-wake left _user_active
        // false (the device never sleeps in that state). All skipped when
        // _screen_off_override is set so the screen stays dark.
        if (!_screen_off_override) {
            if ((_always_on && _is_charging) || (_user_active && _isInhibited())) {
                _setDisplay(DisplayState::ON);
            } else if (_user_active) {
                uint32_t remaining = _calcRemainingS(now);
                if (remaining > 0 && remaining <= 5) _setDisplay(DisplayState::DIM);
                else if (remaining > 5)              _setDisplay(DisplayState::ON);
            }
        }

        // R4 settled-voltage loop: after a charge edge the node keeps slewing
        // for seconds (unplug leaves the rail cap bleeding through R4) and
        // sampling it mid-decay reads garbage — the 250 ms detector above
        // already flipped the state/icon, so the voltage re-read simply waits
        // here until two consecutive 1 s reads agree. A stuck-unsettled node
        // (noise) is force-sampled after 10 polls.
        if (_vsense_5v_divider) {
            uint16_t poll_mv  = g_hal.readVsenseMv();
            uint16_t delta_mv = (poll_mv > _last_poll_mv) ? (uint16_t)(poll_mv - _last_poll_mv)
                                                          : (uint16_t)(_last_poll_mv - poll_mv);
            bool stable   = _have_poll_mv && delta_mv <= PM_R4_SETTLE_DELTA_MV;
            _last_poll_mv = poll_mv;
            _have_poll_mv = true;

            if (_settle_pending && (stable || ++_settle_polls >= 10)) {
                _settle_pending = false;
                _last_batt_ms   = now;   // reset interval to avoid double-sample
                _sampleBattery();
            }
        }
    }

    // Periodic battery sample. Held off while an edge settle-wait is pending
    // (the deferred edge sample lands within seconds and covers it).
    if (!_settle_pending && now - _last_batt_ms >= BATTERY_SAMPLE_INTERVAL_MS) {
        _last_batt_ms = now;
        _sampleBattery();
    }

    // React to a USB-data attach/detach rather than waiting 30 s. Tracked
    // against its own edge flag (not _is_charging): on R4 boards _is_charging
    // is ADC-derived and legitimately differs from usbAttached() for dumb
    // chargers, which would otherwise resample every tick. On R4 boards the
    // node slews for seconds around the edge, so route through the same
    // settle-wait as the threshold poll instead of sampling immediately.
    bool usb_now = g_hal.usbAttached();
    if (usb_now != _last_usb_seen) {
        _last_usb_seen = usb_now;
        if (_vsense_5v_divider) {
            if (!_settle_pending) {
                _settle_pending = true;
                _settle_polls   = 0;
            }
        } else {
            _last_batt_ms = now;  // reset interval to avoid double-sample
            _sampleBattery();
        }
    }

    // While hunting, ScanEngine owns the radio directly (lock-on) and _isInhibited
    // keeps us awake — suspend the whole duty-cycle state machine: no window stop,
    // no drain gate, no sleep, no re-open. The window resumes on CMD_SCAN_LOCKON_STOP.
    if (_hunting) return;

    // Tutorial gate: until the tutorial is completed the device must not scan or
    // arm the scan-cadence timer. An unboxed unit that's accidentally woken would
    // otherwise duty-cycle through wake/sleep scan windows forever. Instead it
    // runs the normal interactive → dim → deep-sleep cycle, but shuts fully off
    // via _enterPowerDown() — a button-only (CENTER-hold) wake with NO timer —
    // so a jostled shipping unit returns to deep sleep on its own to save battery.
    // Inhibit (USB / serial / charging) freezes the timeout, same as normal.
    // This bypasses the whole scan state machine below, so no CMD_SCAN_START /
    // CMD_SCAN_STOP is posted while the tutorial is pending.
    if (!g_settings.getBool(SKEY_TUTORIAL_SHOWN)) {
        if (_isInhibited()) {
            _last_activity_ms = now;
        } else if ((now - _last_activity_ms) >= (uint32_t)_interactive_timeout_s * 1000) {
            _enterPowerDown();   // button-only wake, no timer; does not return
        }
        return;
    }

    // Tutorial just completed (flag flipped while awake, no reboot) — kick off
    // the first scan window now so duty-cycling starts immediately. Reset the
    // window bookkeeping so the state machine below opens a clean window from
    // this moment rather than acting on the stale boot-time _wake_ms.
    if (!_scanning_started) {
        _scanning_started     = true;
        _scan_stop_posted     = false;
        _scan_complete_posted = false;
        _wake_ms              = now;
        _bus->post(CMD_SCAN_START);
    }

    // End of scan window — fire CMD_SCAN_STOP once. The window spans every
    // enabled radio's phase (BLE then WiFi), so it's duration × radio-count.
    if (!_scan_stop_posted &&
        (now - _wake_ms) >= _scanWindowMs()) {
        _scan_stop_posted = true;
        _stop_ms          = now;
        _bus->post(CMD_SCAN_STOP);
    }

    // Scan window still open — nothing more to decide this tick.
    if (!_scan_stop_posted) return;

    // After scan stop: wait for the ScanEngine queue + ScanService ring to
    // drain so every advertisement caught this window has been seen by the
    // rules engine before we sleep / start the next cycle.
    const uint32_t ring_pending  = g_scan_service.writePos() - g_rules.ringReadPos();
    const size_t   queue_pending = g_scan_engine.queueDepth();
    // Don't sleep while a WiFi sweep is still in flight — CMD_SCAN_STOP aborts
    // it, but this closes the narrow same-tick gap before that's dispatched.
    if (ring_pending != 0 || queue_pending != 0 || g_scan_engine.wifiBusy()) return;

    // Drain complete — every advertisement caught this window is now in the
    // seen-devices map (and seen by the rules engine). Fire EV_SCAN_COMPLETE
    // once so the device list can refresh against a fully-populated map.
    if (!_scan_complete_posted) {
        _scan_complete_posted = true;
        _bus->post(EV_SCAN_COMPLETE);
    }

    // Drain complete. Decide what's next:
    //   - autonomous (no user activity, no alert, not on USB/serial) → deep sleep
    //   - everything else (interactive, inhibited, or alert fired) → stay
    //     awake and start the next scan cycle after _sleep_duration_s elapses
    const bool stays_awake = _user_active || _isInhibited();

    if (!stays_awake) {
        _enterSleep();
        return;
    }

    // Interactive timeout — only fires when on battery (inhibit freezes the
    // clock). Without an alert or further button press, the device eventually
    // sleeps on its own.
    if (_isInhibited()) {
        _last_activity_ms = now;
    } else if ((now - _last_activity_ms) >= (uint32_t)_interactive_timeout_s * 1000) {
        _enterSleep();
        return;
    }

    // Wait _sleep_duration_s between scan windows, then open the next one
    // in place (no deep sleep — we're staying awake on purpose). Always-On while
    // charging drops the gap to ~0, so scanning is effectively continuous.
    uint32_t gap_ms = (_always_on && _is_charging) ? 0u
                                                   : (uint32_t)_sleep_duration_s * 1000u;
    if ((now - _stop_ms) >= gap_ms) {
        _scan_stop_posted     = false;
        _scan_complete_posted = false;
        _wake_ms              = now;
        _bus->post(CMD_SCAN_START);
    }
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void PowerManager::onEvent(const Event& e) {
    switch (e.id) {
        case EV_UI_ACTIVITY:
            _user_active      = true;
            _last_activity_ms = millis();
            if (!_screen_off_override) {
                _setDisplay(DisplayState::ON);
            }
            break;

        case EV_BTN_LEFT:
        case EV_BTN_RIGHT:
        case EV_BTN_CENTER_SHORT:
        case EV_BTN_CENTER_LONG:
        case EV_BTN_WAKE:   // dark-screen press HAL swallowed — wake is its whole job
            _screen_off_override = false;
            _user_active         = true;
            _last_activity_ms    = millis();
            _setDisplay(DisplayState::ON);
            break;

        case EV_ALERT_RAISED:
            // Alerts always keep the device awake — without this, an alert
            // fired during the post-scan drain would sleep immediately and
            // the haptic/LED/screen reactions would never get to play. The
            // interactive timeout still applies, so a quiet device returns
            // to sleep on its own. Wake-screen behavior remains gated by
            // SKEY_ALERT_WAKE_SCREEN.
            _user_active      = true;
            _last_activity_ms = millis();
            if (g_settings.getBool(SKEY_ALERT_WAKE_SCREEN)) {
                _screen_off_override = false;
                _setDisplay(DisplayState::ON);
            }
            break;

        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_POWER) {
                _syncSettings();
            }
            break;

        case CMD_POWER_SLEEP:
            _enterSleep();
            break;

        case CMD_POWER_SHIPPING_RESET:
            _factoryResetAndShip();
            break;

        case CMD_POWER_FACTORY_RESET:
            _factoryResetAndReboot();
            break;

        case CMD_POWER_SCREEN_OFF:
            sleepScreen();
            break;

        case CMD_POWER_REBOOT:
            _reboot();
            break;

        case CMD_POWER_DOWN:
            _enterPowerDown();
            break;

        case CMD_SCAN_LOCKON_START:
            // Hunting keeps the device awake (see _isInhibited) and the duty-cycle
            // machine suspended (see tick). Count it as activity so the interactive
            // timeout can't fire the instant the hunt ends.
            _hunting          = true;
            _user_active      = true;
            _last_activity_ms = millis();
            break;

        case CMD_SCAN_LOCKON_STOP:
            // Resume normal scanning with a FRESH window immediately, so the
            // (now-stale) device list re-populates as soon as the user backs out
            // of the foxhunt screen.
            _hunting              = false;
            _user_active          = true;
            _last_activity_ms     = millis();
            _wake_ms              = millis();
            _stop_ms              = millis();
            _scan_stop_posted     = false;
            _scan_complete_posted = false;
            _bus->post(CMD_SCAN_START);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

uint32_t PowerManager::_scanWindowMs() const {
    // Radios are time-multiplexed, not concurrent: each enabled radio owns the
    // window for _scan_duration_s in turn (ScanEngine sequences them). Total
    // window = duration × count. With no enabled radio, the policy returns a
    // zero-length window.
    return scan_power_policy::windowMs(
        g_settings.get(SKEY_SCAN_MODE),
        _scan_duration_s
    );
}

void PowerManager::_syncSettings() {
    _scan_duration_s         = g_settings.getU16(SKEY_SCAN_DURATION_S);
    _sleep_duration_s        = g_settings.getU16(SKEY_SLEEP_DURATION_S);
    _interactive_timeout_s   = g_settings.getU16(SKEY_INTERACTIVE_TIMEOUT_S);
    _sleep_w_serial          = g_settings.getBool(SKEY_DEBUG_SLEEP_WITH_SERIAL);
    _sleep_while_usb         = g_settings.getBool(SKEY_SLEEP_WHILE_USB);
    _sleep_while_charging    = g_settings.getBool(SKEY_SLEEP_WHILE_CHARGING);
    _always_on               = (g_settings.get(SKEY_PERF_MODE) == PERF_ALWAYS_ON);
    _vsense_5v_divider       = g_settings.getBool(SKEY_VSENSE_5V_DIVIDER);
    // Only the R4 threshold poll clears a pending settle-wait; if the divider
    // setting is switched off mid-wait it would gate the periodic sample forever.
    if (!_vsense_5v_divider) _settle_pending = false;

    if (_scan_duration_s       == 0) _scan_duration_s       = 5;
    if (_sleep_duration_s      == 0) _sleep_duration_s      = 30;
    if (_interactive_timeout_s == 0) _interactive_timeout_s = 30;
}

void PowerManager::_sampleBattery() {
    uint16_t vsense       = g_hal.readVsenseMv();
    bool     was_charging = _is_charging;

    // Charge state and VBATT depend on which divider is fitted (see the block
    // in power_manager.h). R4 boards read everything off the node itself;
    // default boards fall back to USB-data presence and a plain 2:1 divider.
    bool     charging;
    uint16_t batt_mv;
    uint16_t line_mv;
    if (_vsense_5v_divider) {
        // VSENSE = (VBATT + V5)/3. On USB the +5V rail lifts a PRESENT pack's
        // node into the charging band; a reading below the charging threshold
        // while USB is attached means nothing bridges VBATT→node → no pack.
        if (g_hal.usbAttached() && vsense < PM_R4_CHARGING_MV) {
            _is_charging       = false;
            _have_smoothed_pct = false;   // restart EMA when a pack reappears
            _last_batt_mv      = 0;
            _last_batt_pct     = BATT_PCT_MISSING;
            EventPayload pm{};
            pm.battery.mv  = 0;
            pm.battery.pct = BATT_PCT_MISSING;
            _bus->post(EV_BATTERY_UPDATED, pm);
            return;
        }

        charging = (vsense > PM_R4_CHARGING_MV);

        // Rail learning is normally armed by _flipChargeState at the edge and
        // consumed here at the first settled charging sample. Catch edges that
        // arrive without a flip (first sample after boot, edge landing during
        // an already-pending settle); any discharge sample ends the session —
        // the RAM value clears, NVS keeps the last-known rail for boot restore.
        if (charging && !was_charging && !_rail_learn_armed) {
            _rail_learn_armed = (_last_batt_pct <= 100 &&
                                 _last_batt_mv >= 3000 && _last_batt_mv <= 4400);
        } else if (!charging) {
            _chg_rail_mv      = 0;
            _rail_learn_armed = false;
        }

        if (charging && _rail_learn_armed) {
            // Learn this charger's rail: the jump in the node reading across
            // the plug-in edge is all rail (varies ~4.7–5.2 V per charger and
            // lands 1:1 on the cell estimate, so no fixed constant can work).
            // _last_batt_mv still holds the pre-plug cell — the edge flip
            // carried it forward unchanged.
            _rail_learn_armed = false;
            uint32_t v3 = (uint32_t)vsense * 3;
            if (v3 > (uint32_t)_last_batt_mv + PM_R4_CHARGE_LIFT_MV) {
                uint32_t rail = v3 - _last_batt_mv - PM_R4_CHARGE_LIFT_MV;
                if (rail >= PM_R4_CHG_RAIL_MIN_MV && rail <= PM_R4_CHG_RAIL_MAX_MV) {
                    _chg_rail_mv = (uint16_t)rail;
                    _persistRail((uint16_t)rail);
                }
            }
        }
        line_mv = pmR4VbattLineMv(vsense, charging ? chargeRailMv()
                                                   : PM_R4_DISCHG_RAIL_MV);
        // Report the cell estimate: strip the charger's ~100mV +BATT lift.
        batt_mv = charging ? (uint16_t)(line_mv - PM_R4_CHARGE_LIFT_MV) : line_mv;
    } else {
        charging = g_hal.usbAttached();
        batt_mv  = (uint16_t)(vsense * 2);
        line_mv  = batt_mv;
    }
    _is_charging = charging;

    // Reset EMA when charging ends: the first discharging reading should not be
    // averaged with stale (inflated) charging-voltage samples.
    if (was_charging && !charging) _have_smoothed_pct = false;

    // Curve LUT is expressed in VSENSE-mV at the VBATT/2 scale.
    uint8_t pct;
    if (charging) {
        // Sentinels carry charge state; the UI re-derives a level glyph from mv.
        // "Full" is only truly confirmed by resampling with charge paused, which
        // the HW gives no way to do — ≥98 % on the curve (line ≥ ~4130 mV) is
        // our best proxy: real terminations read 4160–4240, while a mid-charge
        // pack only gets there with the cell ≥ ~4.05 V. Gate on the pre-lift
        // LINE value: the lift-corrected cell estimate tops out ~91 % at
        // termination and would never latch CHARGED.
        uint8_t line_pct = pmBattPctFromVsenseMv((uint16_t)(line_mv / 2));
        pct = (line_pct >= 98) ? BATT_PCT_CHARGED : BATT_PCT_CHARGING;
    } else {
        uint8_t raw_pct = pmBattPctFromVsenseMv((uint16_t)(batt_mv / 2));
        if (_have_smoothed_pct) {
            pct = (uint8_t)(((uint32_t)_smoothed_pct * 7 + raw_pct + 4) / 8);
        } else {
            pct = raw_pct;
            _have_smoothed_pct = true;
        }
        _smoothed_pct = pct;
    }

    _last_batt_mv  = batt_mv;
    _last_batt_pct = pct;

    EventPayload p{};
    p.battery.mv  = batt_mv;
    p.battery.pct = pct;
    _bus->post(EV_BATTERY_UPDATED, p);
}

void PowerManager::_flipChargeState(bool charging_now) {
    if (charging_now == _is_charging) return;
    _is_charging = charging_now;

    // PERF_ALWAYS_ON is conditional on external power (see secondsUntilNextScan
    // and _isInhibited), so plugging or unplugging silently turns continuous
    // scanning on or off. Say so — otherwise the mode reads as broken on battery.
    // Only reached on a real edge: _sampleBattery sets _is_charging directly at
    // boot, so a unit powered up already plugged in doesn't announce anything.
    if (_always_on) {
        g_toast.show(charging_now ? "Always-on scanning active"
                                  : "Always-on paused\nNeeds external power");
    }

    if (charging_now) {
        // Arm rail learning for the settled sample — valid only when the
        // pre-plug reading is a real discharge percentage.
        _rail_learn_armed = (_last_batt_pct <= 100 &&
                             _last_batt_mv >= 3000 && _last_batt_mv <= 4400);
    } else {
        _chg_rail_mv       = 0;      // session over — NVS keeps the last-known rail
        _rail_learn_armed  = false;
        _have_smoothed_pct = false;  // EMA restarts from the first settled discharge sample
    }

    // Repost with the carried cell estimate — the cell can't have moved across
    // a plug/unplug edge, and the node itself is mid-slew and unreadable. With
    // no usable prior (fresh boot, was MISSING) the settled sample posts instead.
    if (_last_batt_mv < 3000 || _last_batt_mv > 4400) return;

    uint8_t pct;
    if (charging_now) {
        // Same CHARGED gate as _sampleBattery: pre-lift line value, ≥98 %.
        uint16_t line_mv = (uint16_t)(_last_batt_mv + PM_R4_CHARGE_LIFT_MV);
        pct = (pmBattPctFromVsenseMv((uint16_t)(line_mv / 2)) >= 98)
                  ? BATT_PCT_CHARGED : BATT_PCT_CHARGING;
    } else {
        pct = pmBattPctFromVsenseMv((uint16_t)(_last_batt_mv / 2));
    }
    _last_batt_pct = pct;

    EventPayload p{};
    p.battery.mv  = _last_batt_mv;
    p.battery.pct = pct;
    _bus->post(EV_BATTERY_UPDATED, p);
}

void PowerManager::_persistRail(uint16_t rail_mv) {
    // Skip the flash write when the newly learned rail is within noise of the
    // stored one — plug events are frequent on a dev bench.
    Preferences prefs;
    if (!prefs.begin("power", /*readOnly=*/false)) return;
    uint16_t stored = prefs.getUShort("rail", 0);
    uint16_t delta  = (stored > rail_mv) ? (uint16_t)(stored - rail_mv)
                                         : (uint16_t)(rail_mv - stored);
    if (delta > 25) prefs.putUShort("rail", rail_mv);
    prefs.end();
}

uint16_t PowerManager::secondsUntilNextScan() const {
    // Scanning off entirely — the cycle still runs but no radio starts.
    if ((g_settings.get(SKEY_SCAN_MODE) & 0x3u) == 0) return 0xFFFFu;
    // Always-On while charging: scanning is effectively continuous.
    if (_always_on && _is_charging) return 0;
    // Scan window currently open (CMD_SCAN_STOP not yet posted this cycle).
    if (!_scan_stop_posted) return 0;
    // Between windows: next CMD_SCAN_START fires at _stop_ms + _sleep_duration_s.
    // (A deep-sleep autonomous cycle re-arms the same timer, so the value holds.)
    uint32_t elapsed = (millis() - _stop_ms) / 1000;
    return (elapsed < _sleep_duration_s)
           ? (uint16_t)(_sleep_duration_s - elapsed) : 0;
}

uint32_t PowerManager::_calcRemainingS(uint32_t now) const {
    if (_user_active) {
        uint32_t elapsed = (now - _last_activity_ms) / 1000;
        return (elapsed < _interactive_timeout_s)
               ? (_interactive_timeout_s - elapsed) : 0;
    }
    const uint32_t window_s = _scanWindowMs() / 1000;
    uint32_t elapsed = (now - _wake_ms) / 1000;
    return (elapsed < window_s) ? (window_s - elapsed) : 0;
}

void PowerManager::_postCountdown(uint32_t now) {
    EventPayload p{};
    // 0xFFFF is the sentinel for "won't sleep" (USB connected / serial inhibit).
    // UIController renders this as "Will not sleep" in the settings screen.
    p.sleep_count.seconds = _isInhibited()
                             ? 0xFFFFu
                             : (uint16_t)_calcRemainingS(now);
    _bus->post(EV_SLEEP_COUNTDOWN_UPDATED, p);
}

bool PowerManager::_isInhibited() {
    // Hysteresis: if any raw condition is currently true, latch the timestamp.
    // For a few seconds after that, keep returning true even if the raw checks
    // momentarily disagree. (USB hosts occasionally pause SOFs and CDC's
    // _connected flag can briefly drop during heavy traffic — without grace,
    // a single tick where both checks read false is enough to fire _enterSleep
    // even though the user is actively using the terminal.)
    static constexpr uint32_t INHIBIT_GRACE_MS = 2000;

    // Each positive-form "allow sleep" flag is inverted into an inhibit:
    //   Serial open + don't-sleep-with-serial        → inhibit
    //   USB data host attached + don't-sleep-on-USB   → inhibit
    //   charging + don't-sleep-while-charging         → inhibit
    //   Always-On mode + charging                     → inhibit (never sleeps on
    //     external power, regardless of the sleep-while-charging toggle)
    //   party mode active → inhibit (keep the show running until it ends)
    //   admin broadcasting → inhibit (deep sleep tears NimBLE down mid-burst)
    //   admin effect active → inhibit (let a received message/LED/beacon finish)
    //   hunting → inhibit (lock-on must keep tracking; sleep would drop the radio)
    //   manual Helga, LED, or Vibes repetition → display/sleep inhibit (intentional foreground; no post-mode grace)
    const bool foreground_mode =
        g_overview_screen.manualPlaybackActive() ||
        g_leds.manualPatternActive() ||
        g_vibe.repeating();
    bool raw = ((bool)Serial && !_sleep_w_serial)
            || (g_hal.usbAttached() && !_sleep_while_usb)
            || (_is_charging && (!_sleep_while_charging || _always_on))
            || g_party.active()
            || g_admin.broadcasting()
            || g_admin.hasActiveEffect()
            || _hunting;

    if (raw) {
        _last_inhibit_seen_ms = millis();
        return true;
    }
    if (foreground_mode) {
        return true;
    }
    if (_last_inhibit_seen_ms != 0 &&
        (millis() - _last_inhibit_seen_ms) < INHIBIT_GRACE_MS) {
        return true;
    }
    return false;
}

void PowerManager::sleepScreen() {
    _screen_off_override = true;
    _setDisplay(DisplayState::OFF);
}

void PowerManager::wakeScreen() {
    // Mirror of the button-press wake path in onEvent.
    _screen_off_override = false;
    _user_active         = true;
    _last_activity_ms    = millis();
    _setDisplay(DisplayState::ON);
}

void PowerManager::requestSleepOrScreenOff() {
    // Confirmation haptic — direct HAL drive that blocks until it completes.
    // An async g_vibe.play() would be cut short: _enterSleep() stops the motor
    // and deep sleep powers down LEDC before the vibe timer could finish.
    g_hal.setVibrate(220);
    delay(70);
    g_hal.stopVibrate();
    if (_isInhibited()) sleepScreen();
    else                _enterSleep();
}

void PowerManager::_setDisplay(DisplayState s) {
    if (s == _disp_state) return;
    _disp_state = s;
    // OFF: skip both the backlight AND LVGL rendering. Saves ~70 % CPU
    // during silent TIMER-wake scan windows, leaving the cycles for scanner
    // / rules engine work. ON/DIM resume rendering immediately.
    switch (s) {
        case DisplayState::OFF:
            g_hal.sleepDisplay();
            g_ui.setRenderEnabled(false);
            break;
        case DisplayState::ON:
            g_hal.wakeDisplay();
            g_ui.setRenderEnabled(true);
            g_logger.applyPerfMonitor();   // re-sync overlay visibility on wake
            break;
        case DisplayState::DIM:
            g_hal.dimDisplay();
            g_ui.setRenderEnabled(true);
            g_logger.applyPerfMonitor();
            break;
    }
}

void PowerManager::_enterSleep() {
    g_settings.flush();   // persist any deferred setting change before we lose RAM
    // Wait for buttons to be released so EXT1 (wake-on-LOW) doesn't fire
    // immediately on the press that triggered this call. No-op when entered
    // from the serial `sleep` command (no button held).
    while (digitalRead(PIN_BTN_1) == LOW || digitalRead(PIN_BTN_2) == LOW) {
        delay(10);
    }

    // Pre-sleep cleanup.
    // Order matters: clearLEDs needs RMT to still own PIN_LED_DATA. Once
    // prepareForSleep runs pinMode(OUTPUT) on it, RMT is detached and
    // FastLED.show() can't push the all-off frame — the LEDs would hang at
    // their last state through deep sleep.
    g_hal.clearLEDs();
    g_hal.prepareForSleep();   // backlight off, LCD sleep, both pads held LOW
    _disp_state = DisplayState::OFF;

    EventPayload p{};
    p.power.state = POWER_SLEEPING;
    _bus->post(EV_POWER_STATE_CHANGED, p);
    _bus->dispatch();   // flush queue before losing power

    // Wake sources:
    //   1. Timer — scheduled scan cycle when a radio is enabled. Stashed in RTC
    //      so a failed wake-hold check (checkWakeHoldOrResleep) can re-arm the
    //      same cadence. Zero-radio mode has no timer and is button-only sleep.
    //   2. GPIO6 (PIN_BTN_1) LOW — left or center press wakes the chip, but
    //      checkWakeHoldOrResleep then requires CENTER held to stay awake.
    //      GPIO43 (PIN_BTN_2) is not RTC-capable on ESP32-S3, so it can't be a
    //      wake source; it's only read after wake to confirm the center hold.
    _deep_sleep_timer_us = scan_power_policy::wakeIntervalUs(
        g_settings.get(SKEY_SCAN_MODE),
        _sleep_duration_s
    );
    if (_deep_sleep_timer_us > 0) {
        esp_sleep_enable_timer_wakeup(_deep_sleep_timer_us);
    }

    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_1, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    esp_deep_sleep_start();
    // Does not return — device resets on wakeup and setup() runs again.
}

// Wipe every piece of user state, returning the device to out-of-box:
// settings to defaults (incl. tutorial flag), user rulesets deleted, every
// rule disabled, alerts + their RTC mirror cleared, seen-devices map cleared,
// admin re-locked. When this returns, the wipe is applied AND persisted —
// callers only decide what happens next (shipping sleep or reboot).
void PowerManager::_wipeUserState() {
    Serial.println("[power] factory reset — wiping user state");
    g_alerts.clearAll();       // active records + RTC mirror; per-(rule,MAC) dedup dies with them
    g_scan_service.clear();    // PSRAM-only (a reboot wipes it anyway); explicit for clarity
    g_rules.factoryReset();    // user rule files + enabled overlay + reload → factory set, all disabled
    g_admin.lock();            // clear the NVS unlock — next owner gets a locked device

    // Settings reset rides the bus: the handler applies defaults to RAM and
    // flushes to NVS (settings_service.cpp CMD_SETTINGS_RESET_DEFAULTS), so
    // dispatch here to make it happen before we return — _reboot() has no
    // dispatch of its own. Never erase settings NVS directly here, or the
    // pre-sleep/pre-reboot flush would write the old RAM values straight back.
    _bus->post(CMD_SETTINGS_RESET_DEFAULTS);
    _bus->dispatch();
}

// Wipe + shipping sleep — the end-of-assembly-line command (`power shipping`).
// Serial-only on purpose; the user-reachable variant is the reboot one below.
void PowerManager::_factoryResetAndShip() {
    _wipeUserState();
    _enterShippingSleep();
}

// Wipe + reboot — the user-reachable "Reset device" action (debug section /
// future Danger-zone). Same wipe; the device comes back up like a first boot
// (tutorial included) instead of ship-sleeping.
void PowerManager::_factoryResetAndReboot() {
    _wipeUserState();
    _reboot();
}

void PowerManager::_enterShippingSleep() {
    // Shipping: no-timer deep sleep AND reset the tutorial flag, so the device
    // greets whoever unboxes it next like a first-time power-on.
    _enterOffSleep(/*reset_tutorial=*/true);
}

void PowerManager::_enterPowerDown() {
    // Power down: the same deliberate button-only deep sleep as shipping, but
    // leaves SKEY_TUTORIAL_SHOWN alone — a user powering off mid-use shouldn't
    // be re-shown the tutorial on the next wake.
    _enterOffSleep(/*reset_tutorial=*/false);
}

// Deep sleep with NO timer wake — only a deliberate CENTER long-hold on
// PIN_BTN_1 (EXT1) returns, enforced by checkWakeHoldOrResleep at next boot.
// _shipping_pending selects the longer hold and keeps the scan-cadence timer
// from being re-armed on a failed hold. `reset_tutorial` is the ONLY thing
// separating shipping from a plain power-down.
void PowerManager::_enterOffSleep(bool reset_tutorial) {
    // Order matters: clearLEDs needs RMT to still own PIN_LED_DATA. Once
    // prepareForSleep runs pinMode(OUTPUT) on it, RMT is detached and
    // FastLED.show() can't push the all-off frame — the LEDs would hang at
    // their last state through deep sleep.
    g_hal.clearLEDs();
    g_hal.prepareForSleep();   // backlight off, LCD sleep, both pads held LOW
    _disp_state = DisplayState::OFF;

    // Mark us as shipping-pending so the next boot demands a long-press
    // before resuming normal operation.
    _shipping_pending = true;

    Serial.println(reset_tutorial
                   ? "[power] shipping sleep — long-press CENTER to wake"
                   : "[power] power down — long-press CENTER to wake");

    if (reset_tutorial) {
        // Reset tutorial flag so the tutorial shows when the device is taken
        // out of shipping mode (next boot after shipping-mode wake is treated
        // like a first-time power-on from the user's perspective).
        EventPayload tp{};
        tp.settings_set.key   = SKEY_TUTORIAL_SHOWN;
        tp.settings_set.value = 0;
        _bus->post(CMD_SETTINGS_SET, tp);
    }

    EventPayload p{};
    p.power.state = POWER_SLEEPING;
    _bus->post(EV_POWER_STATE_CHANGED, p);
    _bus->dispatch();   // flush queue before losing power
    g_settings.flush(); // commit the tutorial reset (if any) + any pending change

    // Wait for buttons to be released, otherwise EXT1 fires immediately and
    // we wake right back up.
    while (digitalRead(PIN_BTN_1) == LOW || digitalRead(PIN_BTN_2) == LOW) {
        delay(10);
    }

    // Wake source: any button press (GPIO6 LOW). NO timer — this sleep never
    // auto-wakes; only a CENTER hold brings it back.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_1, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    esp_deep_sleep_start();
    // Does not return — device resets on wake and setup() runs from scratch.
}

void PowerManager::_reboot() {
    g_settings.flush();   // persist any deferred setting change before the reset
    // Tear down before the software reset, in layer order. LEDC keeps driving
    // its last PWM duty across a software reset until HAL re-inits it at boot,
    // so without this a mid-play haptic (or the backlight) rides through the
    // whole boot window — that's what left the motor buzzing after a reboot.
    //   • VibeService cancels its own pattern timer (HAL can't reach into it).
    //   • HAL drives the raw peripherals to a safe state (motor / LEDs / BL).
    g_vibe.stop();
    g_hal.prepareForReboot();
    delay(50);          // let register writes land + flush any serial response
    ESP.restart();
    // Does not return.
}
