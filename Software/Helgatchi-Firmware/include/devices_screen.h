#pragma once
#include "event_bus.h"

// DevicesScreen
//
// UI side of the scanned-device list, built as a RECYCLER: a fixed pool of
// ~12 card widgets binds to a sliding window of a data-only row list
// (snapshot of ScanService's dedup'd seen-map, RSSI-strongest-first). Two
// invisible spacers keep the flex column's height identical to a full
// one-card-per-device list, so scroll geometry is continuous and LVGL's
// per-frame cost is constant no matter how many devices are seen.
//
// Rows rebuild on EV_SCAN_COMPLETE only (posted by PowerManager once a scan
// window's backlog has fully drained) — dBm values move only when a scan
// lands. A 5 s timer re-renders the "Ns ago" age on bound cards. Selection
// is a row index pinned by (domain, MAC) across re-sorts, driven directly by
// EV_BTN_* events; the LVGL nav group stays empty on this screen.
//
// Layered like AlertsScreen: ScanService is the LVGL-free data store; this is
// the presentation layer. Initialize AFTER g_ui (objects.* must exist) and
// AFTER g_scan_service.

class DevicesScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

    // Number of devices currently listed (data rows, not pooled widgets) —
    // surfaced for perf telemetry (DEBUG_PERF).
    uint16_t cardCount() const;

    // Open the device-detail overlay for an explicit device instead of the
    // list's own selection — how AlertsScreen walks an alert card back to the
    // device that fired it. The overlay lives on the LVGL top layer and owns
    // the buttons until it closes, so it works over any screen.
    //
    // `context` (may be null) is rendered as a leading line naming what pointed
    // here (the alert title, for an alert card). Returns false when the device
    // has aged out of the scan service's seen map — there's nothing to show
    // then — or when the overlay is already open.
    bool openDeviceDetail(uint8_t domain, const uint8_t mac[6],
                          const char* context = nullptr);
};

extern DevicesScreen g_devices_screen;
