#pragma once
#include "event_bus.h"
#include <lvgl.h>
#include <stdint.h>

// FoxhuntingScreen
//
// Controller for the EEZ "Foxhunting Menu" screen (SCREEN_ID_FOXHUNTING_MENU).
// Entered from the device-detail popup's "Hunt" button via startHunt(): it
// snapshots the target's static fields (name, MAC, vendor), pushes the screen,
// and asks ScanEngine to lock onto that one device (CMD_SCAN_LOCKON_START).
//
// While the screen is up, a poll timer reads the live RSSI/last-seen ScanEngine
// keeps refreshing for the target and drives the RSSI label, the "last seen"
// text, and the 0-100 signal-quality bar (RSSI -100..-30 dBm → 0..100). All
// other scanning is suspended during the hunt.
//
// Exit is the generic back-nav (long-press, handled by UIController): the
// screen's SCREEN_UNLOAD_START stops the hunt (CMD_SCAN_LOCKON_STOP), which
// resumes normal scanning with a fresh window so the device list re-populates.
//
// The EEZ tick for this screen only drives the top bar, so the five data labels
// are ours to own directly — no flow-driven text to fight (unlike the devices
// title). Initialize AFTER g_ui (objects.* must exist), g_scan_service, and
// g_scan_engine.

class FoxhuntingScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;   // unused; no subscriptions

    // Begin hunting the given device: snapshot its details, navigate to the
    // foxhunt screen, and start lock-on. No-op if the device isn't in the seen
    // map (aged out between selection and press).
    void startHunt(uint8_t domain, const uint8_t mac[6]);

    // Stop hunting and resume normal scanning. Called by the screen's
    // SCREEN_UNLOAD_START handler (any way of leaving the screen ends the hunt).
    void stopHunt();

private:
    EventBus* _bus = nullptr;

    bool     _active = false;
    uint8_t  _domain = 0;
    uint8_t  _mac[6] = {0};

    // Snapshot captured at hunt start — shown until the first live sighting, and
    // the source of the static name/details for the whole hunt.
    int8_t   _snap_rssi    = 0;
    uint32_t _snap_last_ms = 0;
    char     _name[40]     = {0};
    char     _details[128] = {0};

    int      _disp_q = 0;   // smoothed quality 0..100 (drives bar length + fill colour + LED)

    void _refresh();                        // pull live RSSI → labels + bar
    static void _pollCb(lv_timer_t* t);     // poll-timer trampoline (guards, then _refresh)
};

extern FoxhuntingScreen g_foxhunting_screen;
