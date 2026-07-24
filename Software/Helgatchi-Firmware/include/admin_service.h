#pragma once
#include "event_bus.h"
#include "admin_types.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// AdminService — HMAC-signed BLE crowd control.
//
// RECEIVE (every device NOT in admin mode): ScanEngine's NimBLE callback filters
// admin adverts into a queue; tick() drains them, authenticates via
// admin_protocol, and runs the idempotent, duration-clamped command (party /
// message box / LED / name-beacon / stop-all). A valid HMAC is the only gate
// to obey — no unlock needed. A device that IS unlocked is a controller and
// ignores received commands (it still drains the queue).
//
// SEND (only when unlocked): `admin unlock <pw>` PBKDF2-checks the password
// against the baked hash and persists the unlocked flag in the "admin" NVS
// namespace. Commands broadcast as non-connectable manufacturer-data adverts.
// A controller ONLY transmits — it never runs commands locally and ignores
// commands received from other controllers (watch a non-admin device to see the
// effect). Every command advertises for a bounded window (BROADCAST_MS, a few
// minutes) then auto-stops; the operator can stop early (the menu stop toggle,
// leaving the menu, or party-stop / stop-all) or re-broadcast to extend. The
// `secs` param is the RECEIVER's action lifetime, NOT the broadcast length. The
// menu card is hidden until unlocked; selecting a command reveals only the
// containers it needs.
//
// Init AFTER g_scan_engine (owns BLE init + the admin queue), LittleFS, and
// g_ui/g_overview (touches objects.*, g_party, g_leds). tick() runs after
// g_scan_engine.tick() so freshly received frames drive effects the same frame.
// ---------------------------------------------------------------------------

class AdminService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;   // unused stub (driven by tick + direct calls)

    // --- Unlock (send authorization) ---
    bool unlock(const char* password);   // PBKDF2 verify; persists to NVS on success
    void lock();                         // clears flag + NVS
    bool unlocked() const { return _unlocked; }
    bool secretIsDefault() const;        // built with dev-default secrets?

    // --- Send (no-op returning false when locked) ---
    // Broadcast a command (serial `admin ...` path): actions advertise
    // continuously, terminators are a bounded burst; never runs locally.
    bool broadcast(AdminCmd cmd, uint8_t param1, uint16_t param2_secs);
    // Stop an in-flight command advert (button toggle / leaving the menu / burst
    // expiry). Safe to call when not broadcasting.
    void stopBroadcast();

    // --- Inspectors for PowerManager sleep-inhibit ---
    bool broadcasting()    const { return _bcasting; }
    bool hasActiveEffect() const;        // message box / admin LED / name beacon pending

    // --- On-device Admin Menu (§11; navigation to the screen is EEZ-Flow-driven) ---
    void onMenuShown();            // SCREEN_LOAD_START: nav-group + initial visibility/label
    void menuCommandChanged();     // command dropdown changed → show/hide the other containers
    void menuToggleBroadcast();    // broadcast button → start/stop the selected command
    void syncCardVisibility();     // main-menu admin card: shown only while unlocked

    // Message table (single source; also used to populate the EEZ dropdown).
    static uint8_t     messageCount();
    static const char* messageText(uint8_t idx);   // "" if out of range

    // Called by the message-box LVGL delete callback (any dismissal route).
    void onMsgboxDeleted();

    // How long a command advertises before auto-stopping (operator can stop early
    // or re-broadcast to extend). A few minutes: overlaps receivers' duty-cycled
    // scan windows without pinning the radio/battery indefinitely. Tune here.
    static constexpr uint32_t BROADCAST_MS = 180000;   // 3 min
    // Per-message manual-dismiss cooldown slots (>= the compiled message count).
    static constexpr uint8_t  MSG_SLOTS    = 16;

private:
    EventBus* _bus      = nullptr;
    bool      _unlocked = false;

    bool      _bcasting         = false; // a command advert is currently active
    uint32_t  _bcast_until_ms   = 0;     // 0 = continuous (menu toggle); else timed-burst deadline

    uint32_t  _beacon_until_ms     = 0;  // 0 = not name-beaconing (receiver side only)

    uint32_t  _led_until_ms     = 0;     // admin LED deadline (for hasActiveEffect / keep-awake)

    // Received MESSAGE: a modal lv_msgbox (like the device-detail popup). secs →
    // auto-dismiss deadline; a long-press dismiss (handled generically in
    // UIController) arms a PER-MESSAGE cooldown so the same message isn't
    // re-shown for a while — other messages still show.
    void*     _msgbox           = nullptr;   // lv_obj_t* modal, or null
    uint32_t  _msg_until_ms     = 0;         // auto-dismiss deadline (0 = none open)
    bool      _msg_auto_close   = false;     // true while C auto-dismisses (skip cooldown)
    uint8_t   _msg_shown_idx    = 0;         // index of the message currently shown
    void*     _msg_saved_group  = nullptr;   // keypad group to restore on close (lv_group_t*)
    uint32_t  _msg_cooldown_until_ms[MSG_SLOTS] = {};  // per-message manual-dismiss lockout

    void     _execute(AdminCmd cmd, uint8_t param1, uint16_t param2_secs);
    void     _startCommandAdvert(const uint8_t* msd, uint32_t len, uint32_t auto_stop_ms);
    void     _startNameBeacon(uint16_t secs);
    void     _advStop();
    void     _showMessage(uint8_t idx, uint32_t dur_ms);
    void     _closeMessage(bool auto_close);   // auto_close=true → self-dismiss (no cooldown)
    void     _applyMenuVisibility();       // show/hide containers for the selected command
    void     _updateBroadcastButtonLabel();
    void     _loadUnlock();
    void     _persistUnlock();
    uint16_t _clampSecs(uint16_t secs) const;
};

extern AdminService g_admin;
