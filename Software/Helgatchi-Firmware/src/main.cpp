#include <Arduino.h>
#include "event_bus.h"
#include "settings_service.h"
#include "hal.h"
#include "log_service.h"
#include "serial_console.h"
#include "power_manager.h"
#include "display_service.h"
#include "settings_screen.h"
#include "alerts_screen.h"
#include "devices_screen.h"
#include "foxhunting_screen.h"
#include "debug_screen.h"
#include "rules_screen.h"
#include "overview_screen.h"
#include "power_menu_screen.h"
#include "party_service.h"
#include "admin_service.h"
#include "ui_controller.h"
#include "ui_boot.h"
#include "led_service.h"
#include "vibe_service.h"
#include "alerts_service.h"
#include "scan_service.h"
#include "scan_engine.h"
#include "rules_service.h"
#include "perf_stats.h"
#include <LittleFS.h>
#include <esp_sleep.h>
#include <esp_system.h>

LoopPerf g_loop_perf;

static void _printBootInfo() {
    Serial.printf("[boot] chip:  %s rev%u  cores:%u\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("[boot] mac:   %02X:%02X:%02X:%02X:%02X:%02X\n",
                  (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
                  (uint8_t)(mac >> 16), (uint8_t)(mac >>  8), (uint8_t)(mac));
    Serial.printf("[boot] heap:  %lu B free\n",  (unsigned long)ESP.getFreeHeap());
    Serial.printf("[boot] flash: %lu KB\n",       (unsigned long)(ESP.getFlashChipSize() / 1024));
    Serial.printf("[boot] scan:  mode=%u  perf=%u  scan_s=%u  sleep_s=%u\n",
                  g_settings.get(SKEY_SCAN_MODE),   g_settings.get(SKEY_PERF_MODE),
                  g_settings.get(SKEY_SCAN_DURATION_S), g_settings.get(SKEY_SLEEP_DURATION_S));
    Serial.printf("[boot] vsense: %u mV\n", g_hal.readVsenseMv());
    Serial.printf("[boot] debug: level=%u  sleep_w_serial=%u\n",
                  g_settings.get(SKEY_DEBUG_LEVEL),
                  g_settings.getBool(SKEY_DEBUG_SLEEP_WITH_SERIAL));
}

void setup() {
    Serial.begin(115200);

    // EARLIEST: on a button wake from deep sleep (regular or shipping), verify
    // the user is holding CENTER long enough — otherwise re-enter the same
    // sleep without spinning anything else up. May not return. Timer wakes and
    // cold boots pass straight through.
    PowerManager::checkWakeHoldOrResleep();

    // Wake cause decides how much of setup() is worth running. Read once here;
    // the boot-indicator block below reuses it.
    const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();

    // Wait for a USB CDC host to attach so the boot log isn't lost — but only
    // when someone could be watching. A TIMER wake is an autonomous scan cycle
    // running on battery with the screen off, where CDC never asserts, so this
    // burned its full 2 s at 240 MHz on EVERY cycle: more than a third of the
    // fixed per-cycle overhead, for a log nobody reads. Cold boot and button
    // wakes still wait, so flashing and interactive debugging are unchanged.
    if (wake_cause != ESP_SLEEP_WAKEUP_TIMER) {
        const uint32_t t0 = millis();
        while (!Serial && (millis() - t0) < 2000) { delay(10); }
        delay(200);
    }

    g_bus.begin();
    g_settings.begin(g_bus);
    g_hal.begin(g_bus);
    g_logger.begin(g_bus);
    g_console.begin(g_bus);
    g_power.begin(g_bus);
    g_alerts.begin(g_bus); // must precede led/vibe so they can find() records when EV_ALERT_RAISED fires
    g_scan_service.begin(g_bus);          // ring buffer + seen-devices map
    g_scan_engine.begin(g_bus);   // NimBLE driver — publishes into g_scan_service
    // LittleFS must be mounted before RulesService reads /rules/factory and
    // /rules/user. formatOnFail=true so a fresh device with no FS image
    // still boots (it'll just find an empty filesystem).
    if (!LittleFS.begin(true /* formatOnFail */)) {
        Serial.println("[fs] FATAL: LittleFS mount failed — rules subsystem disabled");
    }
    g_rules.begin(g_bus);  // must follow LittleFS mount + g_scan_service + g_alerts
    g_leds.begin(g_bus);   // depends on HAL (LED chain) + bus events from PowerManager
    g_vibe.begin(g_bus);   // haptic patterns; subscribes to button + alert events
    g_admin.begin(g_bus);  // admin receiver — must follow g_scan_engine (BLE init + admin queue)
    g_party.begin(g_bus);  // party state — a rule can fire it during a headless window, so not UI-gated

    // UI stack (LVGL + EEZ screens + every screen service) — deferred, see
    // ui_boot.h. g_power.begin() above already resolved the display state from
    // the wake cause, so a cold boot or button wake has requested it by now and
    // this builds it in place; an autonomous scan wake leaves it down and pays
    // none of it. Runs here so the ordering the screen begin()s depend on
    // (g_alerts, g_scan_service, g_scan_engine, g_rules) is unchanged.
    uiBringUpNow(g_bus);

    if (g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED)) {
        _printBootInfo();
    }

    // Boot indicator: white LED flash + short haptic. Only fires for boots
    // the user *initiated* — fresh power-on, or button wake from deep sleep.
    // Software resets (Reboot button calling ESP.restart, panic, watchdog)
    // get NO indicator: the user just produced a haptic clicking the button
    // that triggered the reset, and a second haptic on the other side feels
    // like one long buzz. TIMER wakes (autonomous scan) also stay silent.
    {
        esp_reset_reason_t       reset  = esp_reset_reason();
        bool show_indicator =
            (reset == ESP_RST_POWERON) ||
            (reset == ESP_RST_DEEPSLEEP && wake_cause == ESP_SLEEP_WAKEUP_EXT1);

        if (show_indicator) {
            g_hal.setAllLEDs(30, 30, 30);
            if (g_settings.getBool(SKEY_ALERT_VIBRATION)) {
                g_hal.setVibrate(220);
                delay(60);
                g_hal.stopVibrate();
                delay(140);
            } else {
                delay(200);
            }
            g_hal.clearLEDs();
        }
    }

    Serial.println("[Helgatchi] boot OK");
}

void loop() {
    // Service a deferred UI bring-up first, outside dispatch(): requests arrive
    // from inside it (an alert waking the screen) and the screen begin()s
    // subscribe to the bus, which dispatch() iterates live. No-op once up.
    if (uiBringUpPending()) uiBringUpNow(g_bus);

    // Per-phase timing folded into g_loop_perf (worst tick per 1 s window). One
    // micros() read per phase — negligible — so it runs unconditionally;
    // LogService reports it only at DEBUG_PERF. See perf_stats.h.
    uint32_t _t = micros();
    const uint32_t _loop_start = _t;

    PERF_TIME(hal_us,     g_hal.tick());         // USB SOF (attach) detection — buttons + haptics run on their own esp_timers
    PERF_TIME(bus_us,     g_bus.dispatch());     // drain event queue and call all handlers (device-list rebuild runs here)
    PERF_TIME(console_us, g_console.tick());     // process any pending serial input
    PERF_TIME(power_us,   g_power.tick());       // scan/sleep cycle + battery sampling
    PERF_TIME(scan_us,    g_scan_engine.tick()); // drain NimBLE callback queue + publish to g_scan_service
    g_admin.tick();                              // drain + auth admin command frames; expire admin effects
    PERF_TIME(rules_us,   g_rules.tick());       // drain scan ring + match against loaded rules
    PERF_TIME(leds_us,    g_leds.tick());        // ~30 FPS LED pattern render (frame-skips internally)
    g_party.tick();                              // party mode: re-fire haptics, cycle banner colour, keep-awake (no-op when idle)
    // lv_timer_handler — drives LVGL rendering. Skipped entirely while the UI is
    // down (headless scan window): lv_init() hasn't run, so there is nothing to
    // tick and calling in would fault.
    if (uiIsUp()) { PERF_TIME(ui_us, g_ui.tick()); }
    // Haptics no longer tick here — VibeService runs its step machine on a
    // one-shot esp_timer, immune to loop-cadence stalls (see vibe_service.h).

    const uint32_t _loop_dt = micros() - _loop_start;
    if (_loop_dt > g_loop_perf.loop_us) g_loop_perf.loop_us = _loop_dt;
    g_loop_perf.iterations++;
}
