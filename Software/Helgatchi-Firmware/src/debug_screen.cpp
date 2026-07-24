#include "debug_screen.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "power_manager.h"
#include "scan_service.h"
#include "scan_types.h"
#include "rules_service.h"
#include "alerts_service.h"
#include "hal.h"
#include "UI/screens.h"
#include <lvgl.h>
#include <Arduino.h>
#include <esp_sleep.h>
#include <stdio.h>

DebugScreen g_debug_screen;

// ---------------------------------------------------------------------------
// Cached value-label handles (right-hand column of each EEZ container). Only
// the System column has a named handle; the rest are child index 1 of their
// container. Resolved once in begin().
// ---------------------------------------------------------------------------

static lv_obj_t*   _sys_vals   = nullptr;
static lv_obj_t*   _power_vals = nullptr;
static lv_obj_t*   _scan_vals  = nullptr;
static lv_obj_t*   _rules_vals = nullptr;
static lv_timer_t* _refresh_timer = nullptr;

// ---------------------------------------------------------------------------
// Formatters
// ---------------------------------------------------------------------------

// Uptime as "1d 2h 3m 4s", dropping leading zero units, always showing secs.
static void _fmtUptime(char* out, size_t sz, uint32_t ms) {
    uint32_t s = ms / 1000;
    uint32_t d = s / 86400; s %= 86400;
    uint32_t h = s / 3600;  s %= 3600;
    uint32_t m = s / 60;    s %= 60;
    char* p = out; int rem = (int)sz; int n;
    if (d)            { n = snprintf(p, rem, "%ud ", (unsigned)d); p += n; rem -= n; }
    if (d || h)       { n = snprintf(p, rem, "%uh ", (unsigned)h); p += n; rem -= n; }
    if (d || h || m)  { n = snprintf(p, rem, "%um ", (unsigned)m); p += n; rem -= n; }
    snprintf(p, rem, "%us", (unsigned)s);
}

// Compact byte count: "512B" / "181K" / "7.8M". No space so pairs stay narrow.
static void _fmtBytes(char* out, size_t sz, uint32_t b) {
    if      (b >= (1U << 20)) { uint32_t x = (uint32_t)(((uint64_t)b * 10) >> 20);
                                snprintf(out, sz, "%u.%uM", (unsigned)(x / 10), (unsigned)(x % 10)); }
    else if (b >= (1U << 10))   snprintf(out, sz, "%uK", (unsigned)(b >> 10));
    else                        snprintf(out, sz, "%uB", (unsigned)b);
}

// "used/total" — the shape the SRAM / PSRAM / LV_MEM lines share.
static void _fmtMemPair(char* out, size_t sz, uint32_t used_b, uint32_t total_b) {
    char u[12], t[12];
    _fmtBytes(u, sizeof(u), used_b);
    _fmtBytes(t, sizeof(t), total_b);
    snprintf(out, sz, "%s/%s", u, t);
}

// "45s" / "2m 5s" countdown.
static void _fmtCountdown(char* out, size_t sz, uint16_t s) {
    if (s >= 60) snprintf(out, sz, "%um %us", s / 60, s % 60);
    else         snprintf(out, sz, "%us", s);
}

static const char* _perfName(uint32_t mode) {
    static const char* const NAMES[] = { "Performance", "Balanced", "Battery Saver", "Dynamic", "Always-on" };
    return mode < PERF_MODE_COUNT ? NAMES[mode] : "?";
}

static const char* _wakeName(esp_sleep_wakeup_cause_t cause) {
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT1:      return "Button";
        case ESP_SLEEP_WAKEUP_TIMER:     return "Scan timer";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "Power-on";
        default:                         return "Reset";
    }
}

// ---------------------------------------------------------------------------
// Populate — reads live services and writes the four value columns.
// ---------------------------------------------------------------------------

static void _populate() {
    char buf[192];

    // --- System & Health (6 lines) ---
    if (_sys_vals) {
        char up[24], sram[24], psram[24], lvmem[24];
        _fmtUptime(up, sizeof(up), millis());
        _fmtMemPair(sram,  sizeof(sram),  ESP.getHeapSize()  - ESP.getFreeHeap(),  ESP.getHeapSize());
        _fmtMemPair(psram, sizeof(psram), ESP.getPsramSize() - ESP.getFreePsram(), ESP.getPsramSize());

        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        _fmtMemPair(lvmem, sizeof(lvmem),
                    (uint32_t)(mon.total_size - mon.free_size), (uint32_t)mon.total_size);

        snprintf(buf, sizeof(buf), "%s\n%s\n%s\n%s\n%lu\n%lu",
                 up, sram, psram, lvmem,
                 (unsigned long)g_bus.eventCount(),
                 (unsigned long)g_bus.droppedCount());
        lv_label_set_text(_sys_vals, buf);
    }

    // --- Power (5 lines) ---
    if (_power_vals) {
        uint16_t vsense = g_hal.readVsenseMv();     // fresh raw ADC (post-divider)
        uint16_t vbatt  = g_power.lastBatteryMv();  // last derived VBATT (30s sample)
        uint8_t  pct    = g_power.lastBatteryPct();

        char pctbuf[16];
        if      (pct == 0xFF)              snprintf(pctbuf, sizeof(pctbuf), "n/a");
        else if (pct == BATT_PCT_CHARGING) snprintf(pctbuf, sizeof(pctbuf), "Charging");
        else if (pct == BATT_PCT_CHARGED)  snprintf(pctbuf, sizeof(pctbuf), "Full (chg)");
        else if (pct == BATT_PCT_MISSING)  snprintf(pctbuf, sizeof(pctbuf), "No batt");
        else                               snprintf(pctbuf, sizeof(pctbuf), "%u%%", pct);

        snprintf(buf, sizeof(buf), "%u mV\n%u mV\n%s\n%s\n%s",
                 vsense, vbatt, pctbuf,
                 _perfName(g_settings.get(SKEY_PERF_MODE)),
                 _wakeName(esp_sleep_get_wakeup_cause()));
        lv_label_set_text(_power_vals, buf);
    }

    // --- Scanning (4 lines) ---
    if (_scan_vals) {
        uint32_t sm = g_settings.get(SKEY_SCAN_MODE) & 0x3u;
        const char* smn = (sm == 0) ? "Off"
                        : (sm == 1) ? "BLE"
                        : (sm == 2) ? "WiFi"
                                    : "BLE + WiFi";

        uint16_t ble = 0, wifi = 0;
        size_t n = g_scan_service.seenCount();
        for (size_t i = 0; i < n; i++) {
            if (g_scan_service.seenAt(i).domain == SCAN_WIFI) wifi++;
            else                                      ble++;
        }

        char nsb[16];
        uint16_t ns = g_power.secondsUntilNextScan();
        if      (ns == 0xFFFFu) snprintf(nsb, sizeof(nsb), "never");
        else if (ns == 0)       snprintf(nsb, sizeof(nsb), "now");
        else                    _fmtCountdown(nsb, sizeof(nsb), ns);

        snprintf(buf, sizeof(buf), "%s\n%u\n%u\n%s", smn, ble, wifi, nsb);
        lv_label_set_text(_scan_vals, buf);
    }

    // --- Rules & Alerts (5 lines) ---
    if (_rules_vals) {
        uint16_t factory_sets = 0, user_sets = 0;
        uint32_t factory_rules = 0, user_rules = 0;
        uint16_t nr = g_rules.count();
        for (uint16_t i = 0; i < nr; i++) {
            const Rule* r = g_rules.get(i);
            if (!r) continue;
            if (r->is_factory) { factory_sets++; factory_rules += r->criterion_count; }
            else               { user_sets++;    user_rules    += r->criterion_count; }
        }

        snprintf(buf, sizeof(buf), "%u\n%u\n%lu\n%lu\n%u",
                 factory_sets, user_sets,
                 (unsigned long)factory_rules, (unsigned long)user_rules,
                 g_alerts.raisedCount());
        lv_label_set_text(_rules_vals, buf);
    }
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void _on_load(lv_event_t* /*e*/) {
    _populate();   // immediate refresh so values are fresh the moment it opens
}

static void _refreshCb(lv_timer_t* /*t*/) {
    // Only touch the labels while the debug screen is actually showing.
    if (lv_scr_act() == objects.debug_info) _populate();
}

// ---------------------------------------------------------------------------
// Lifecycle — call after g_ui.begin() so objects.* are valid.
// ---------------------------------------------------------------------------

void DebugScreen::begin(EventBus& /*bus*/) {
    if (!objects.debug_info) return;

    // Each value column is child index 1 (the right-aligned label) of its EEZ
    // container; child 0 is the static key label.
    _sys_vals   = objects.system___health_container
                    ? lv_obj_get_child(objects.system___health_container, 1) : nullptr;
    _power_vals = objects.power_container
                    ? lv_obj_get_child(objects.power_container, 1) : nullptr;
    _scan_vals  = objects.scanning_container
                    ? lv_obj_get_child(objects.scanning_container, 1) : nullptr;
    _rules_vals = objects.rules___alerts_container
                    ? lv_obj_get_child(objects.rules___alerts_container, 1) : nullptr;

    lv_obj_add_event_cb(objects.debug_info, _on_load, LV_EVENT_SCREEN_LOAD_START, nullptr);
    _refresh_timer = lv_timer_create(_refreshCb, 1000, nullptr);
}
