#pragma once
#include "event_bus.h"
#include "settings_service.h"

class LogService : public IEventHandler {
public:
    // Call after g_settings.begin(). Uses subscribeAll — one bus slot.
    void begin(EventBus& bus);

    // IEventHandler
    void onEvent(const Event& e) override;

    // Re-evaluate the LVGL FPS overlay's visibility against the current
    // debug level. Must be called any time the LVGL display might have been
    // (re)created or when render is being re-enabled — LVGL auto-shows the
    // overlay when LV_USE_PERF_MONITOR=1 in lv_display_create(), so we have
    // to actively re-hide it after init unless we're at RENDERING_PERF+.
    void applyPerfMonitor() { _applyPerfMonitor(); }

    // Register the LVGL log print callback. Must be called AFTER lv_init /
    // g_ui.begin (lv_init would otherwise clear the registration). LVGL log
    // output is gated to the DEBUG_RENDERING_PERF level.
    void attachLvglLog();

private:
    void _syncSettings();
    void _applyPerfMonitor();    // show/hide LVGL FPS overlay based on level
    void _emitPerfLine();        // one-line render/bus summary at RENDERING_PERF
    void _emitPerfTelemetry();   // memory + scan-pressure + loop-timing (human) at DEBUG_PERF
    void _emitTeleplot();        // Teleplot ">k:v" graphing stream at DEBUG_TELEPLOT
    static const char* _eventName(EventId id);

    bool       _enabled     = false;
    DebugLevel _debug_level = DEBUG_INFORMATIONAL;

    // Deltas for per-second rate reporting in the DEBUG_PERF / DEBUG_TELEPLOT
    // telemetry emitters (only one level is active at a time, so they share).
    uint32_t   _last_cb     = 0;   // g_scan_engine.callbacks() at last emit
    uint32_t   _last_pub    = 0;   // g_scan_engine.published() at last emit
    uint32_t   _last_wifi_res = 0; // g_scan_engine.wifiResults() at last emit — for wifi AP rate
    uint32_t   _last_bus_ev = 0;   // g_bus.eventCount() at last emit — for bus event rate
    uint32_t   _last_frames = 0;   // g_ui.frameCount() at last emit — for ui_fps
    uint32_t   _last_perf_ms = 0;  // millis() at last emit — for loop_hz over real elapsed
};

extern LogService g_logger;
