#pragma once
#include "event_bus.h"

class DisplayService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

    // Rebuilds the status-bar status_icons string (BT / WiFi / Bell). Called
    // from AlertsScreen when alerts come and go, since the bell glyph
    // depends on g_alerts.count(). DisplayService also calls it internally
    // on relevant settings changes.
    void refreshStatusIcons();

    // Party mode: tint every top-bar glyph (left icons + right battery/prefix)
    // a single colour instead of their status colours, and repaint. Party drives
    // setIconTint() on a hue cycle each frame; clearIconTint() restores normal
    // status colouring. The tint is also honoured by the internal event-driven
    // refreshes so a battery/scan update mid-party doesn't flash back to normal.
    void setIconTint(uint32_t rgb);
    void clearIconTint();

    // Foxhunt mode: while on, the left status icons collapse to just a GPS glyph
    // followed by the ONE hunted radio's icon (BT or WiFi), both in the active-
    // scan colour, instead of the normal per-domain scan icons. FoxhuntingScreen
    // toggles this on hunt start/stop; setHunt repaints immediately.
    void setHunt(bool on, uint8_t domain);   // domain = ScanDomain (SCAN_BLE / SCAN_WIFI)

private:
    EventBus* _bus           = nullptr;
    uint16_t  _last_batt_mv  = 0;
    uint8_t   _last_batt_pct = 0xFF;
    bool      _ble_scanning  = false;   // BT icon blue while true  (EV_SCAN_STATE_CHANGED, SCAN_BLE)
    bool      _wifi_scanning = false;   // WiFi icon blue while true (EV_SCAN_STATE_CHANGED, SCAN_WIFI)
    bool      _hunting       = false;   // foxhunt status-icon override active
    uint8_t   _hunt_domain   = 0;       // ScanDomain being hunted (which icon to show)
};

extern DisplayService g_display;
