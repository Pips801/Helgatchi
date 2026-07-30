#pragma once
#include "event_bus.h"
#include <lvgl.h>

class UIController : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // PowerManager calls this from _setDisplay so we skip lv_timer_handler
    // when the screen is off — saves the ~70 % CPU LVGL spends rendering
    // invisible frames during silent (TIMER-wake) scan windows.
    void setRenderEnabled(bool enabled) { _render_enabled = enabled; }

    // Ground-truth display flush rate, independent of LVGL's perf overlay.
    // Returns the number of flush_cb invocations since the previous call to
    // this function plus the elapsed milliseconds since that call. Resets
    // the internal counter; first call reports stats since boot.
    void getDisplayStats(uint32_t& flushes_out, uint32_t& elapsed_ms_out);

    // Worst-frame split of the UI phase since the last call (resets on read):
    // render = LVGL rasterization (+ EEZ flow), flush = SPI/DMA drain + PSRAM
    // cache writeback. render + flush ≈ the worst phase_ui frame — the ratio
    // says whether the UI is rasterization- or transfer-bound. Fed to teleplot.
    void getRenderSplit(uint32_t& render_max_us, uint32_t& flush_max_us);

    // Cumulative count of completed LVGL display-refresh cycles (LV_EVENT_
    // REFR_READY) since boot — the events the perf-monitor FPS counts. Delta it
    // against elapsed time for frames-per-second. Never resets.
    uint32_t frameCount() const;

    // Show the "updating firmware" screen and force it onto the panel
    // immediately. Called just before a web-serial flash begins: the last
    // framebuffer persists through flashing until the device resets, so the
    // device must render this itself first. Returns after the pixels are
    // flushed. (Will load a dedicated EEZ screen once that's added.)
    void showUpdatingScreen();

    // Park / restore keypad routing while a modal owns the buttons.
    //
    // A modal (the device-detail overlay) drives its own navigation from
    // EV_BTN_* directly, so LVGL must not also step focus through the screen
    // behind it. captureKeypad points the indev at a private empty group —
    // lv_indev's keypad path drops keys when the focused object is null — and
    // releaseKeypad points it back at groups.UINavigation. The screen's own
    // group and its focused widget are never touched, so nothing has to be
    // rebuilt or re-focused on close.
    //
    // Idempotent in both directions, and NOT nesting-aware: there is one modal
    // in the UI and it's a singleton. A second concurrent modal would need a
    // depth count here.
    void captureKeypad();
    void releaseKeypad();

private:
    EventBus* _bus = nullptr;
    bool      _render_enabled = true;
};

extern UIController g_ui;
