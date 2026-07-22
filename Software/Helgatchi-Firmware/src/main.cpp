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

    const uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }
    delay(200);

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
    g_ui.begin(g_bus);     // creates the LVGL display — auto-shows perf overlay
    g_logger.attachLvglLog(); // route LVGL logs to serial (Render debug level) — must follow lv_init
    g_display.begin(g_bus); // top-bar indicators — must follow g_ui (objects.* must exist)
    g_settings_screen.begin(g_bus); // settings widget wiring — must follow g_ui
    g_alerts_screen.begin(g_bus);   // alert cards UI — must follow g_ui + g_display + g_alerts
    g_devices_screen.begin(g_bus);  // device cards UI — must follow g_ui + g_scan_service
    g_foxhunting_screen.begin(g_bus); // foxhunt lock-on UI — must follow g_ui + g_scan_service + g_scan_engine
    g_debug_screen.begin(g_bus);    // diagnostics view — must follow g_ui
    g_rules_screen.begin(g_bus);    // rule cards UI — must follow g_ui + g_rules
    g_overview_screen.begin(g_bus); // Helga character animation — must follow g_ui
    g_party.begin(g_bus);           // party mode — must follow g_ui + g_overview_screen (references objects.*)
    g_admin.begin(g_bus);           // admin mode — must follow g_scan_engine (BLE init + admin queue) + g_ui (objects.*)
    g_power_menu_screen.begin(g_bus); // power menu buttons + sleep countdown — must follow g_ui
    g_logger.applyPerfMonitor();   // re-hide unless level >= RENDERING_PERF

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
        esp_sleep_wakeup_cause_t cause  = esp_sleep_get_wakeup_cause();
        esp_reset_reason_t       reset  = esp_reset_reason();
        bool show_indicator =
            (reset == ESP_RST_POWERON) ||
            (reset == ESP_RST_DEEPSLEEP && cause == ESP_SLEEP_WAKEUP_EXT1);

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
    PERF_TIME(ui_us,      g_ui.tick());          // lv_timer_handler — drives LVGL rendering
    // Haptics no longer tick here — VibeService runs its step machine on a
    // one-shot esp_timer, immune to loop-cadence stalls (see vibe_service.h).

    const uint32_t _loop_dt = micros() - _loop_start;
    if (_loop_dt > g_loop_perf.loop_us) g_loop_perf.loop_us = _loop_dt;
    g_loop_perf.iterations++;
}
