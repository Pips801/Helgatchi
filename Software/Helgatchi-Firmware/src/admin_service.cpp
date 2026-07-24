#include "admin_service.h"
#include "admin_protocol.h"
#include "admin_crypto.h"
#include "scan_engine.h"
#include "party_service.h"
#include "led_service.h"
#include "display_service.h"   // admin-mode top-bar icon refresh
#include "event_ids.h"
#include "UI/screens.h"     // objects, groups
#include "UI/eez-flow.h"    // eez_flow_pop_screen (bail out of the menu on lock)
#include <Arduino.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include <lvgl.h>
#include <string.h>

AdminService g_admin;

// -----------------------------------------------------------------------------

namespace {

// Single-source message table. The sender's dropdown index and the receiver's
// messageText() both read this array, so indices agree across identically-built
// devices. (Migrate to a data/ file when the web-flasher message editor lands.)
const char* const ADMIN_MESSAGES[] = {
    "MARV WAS HERE",
    "Hello world!",
    "PIPS WAS HERE",
    "Helga loves you!",
    "Thanks for coming!",
    "Party time!",
    "Goodbye!",
    "See you next time!",
    "Are you having fun?",
    "Task failed successfully",
    "I am a teapot",
    "You've got mail!",
    "Made you look!",
    "BAAAAAAAAAAAA"   
};
constexpr uint8_t ADMIN_MESSAGE_COUNT = sizeof(ADMIN_MESSAGES) / sizeof(ADMIN_MESSAGES[0]);
static_assert(ADMIN_MESSAGE_COUNT <= AdminService::MSG_SLOTS,
              "add more per-message cooldown slots (AdminService::MSG_SLOTS)");

const char* const ADMIN_NVS_NS  = "admin";
const char* const ADMIN_NVS_KEY = "unlk";
const char* const ADMIN_NVS_FP  = "fp";     // secret fingerprint bound to the flag

// Fast advertising interval so several packets land in a receiver's short scan
// window (units of 0.625 ms → 100–200 ms).
constexpr uint16_t ADV_ITVL_MIN = 0x00A0;
constexpr uint16_t ADV_ITVL_MAX = 0x0140;

// Null-safe show/hide.
void setHidden(lv_obj_t* o, bool hidden) {
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Maps the EEZ admin_duration_dropdown index to seconds. LOCKED to the dropdown
// option order: "5s / 10s / 20s / 30s / 1m / 3m / 5m / forever".
uint16_t durationFromIndex(uint32_t i) {
    static const uint16_t T[] = { 5, 10, 20, 30, 60, 180, 300, ADMIN_DUR_FOREVER };
    return i < (sizeof(T) / sizeof(T[0])) ? T[i] : 20;
}

// Advertising TX power for admin sends (advertising only; scan-request power is
// left alone). Deliberately NOT the P21 ceiling — that's at/above the XIAO S3's
// limit and unreliable; +9 dBm is a well-supported level with good range. Tune here.
constexpr esp_power_level_t ADMIN_ADV_TX_LEVEL = ESP_PWR_LVL_P9;

// The keypad input device (buttons), so a modal message box can suspend the
// underlying screen's navigation while it's up.
lv_indev_t* kbdIndev() {
    lv_indev_t* d = nullptr;
    while ((d = lv_indev_get_next(d)) != nullptr)
        if (lv_indev_get_type(d) == LV_INDEV_TYPE_KEYPAD) return d;
    return nullptr;
}

// Deduped receive-side diagnostic log. Admin frames rebroadcast continuously, so
// only log when the outcome string changes (its literal pointer is stable) to
// avoid spamming the console. Makes "why isn't this device reacting?" visible.
const char* s_rx_last = nullptr;
void rxLog(const char* msg) {
    if (msg == s_rx_last) return;
    s_rx_last = msg;
    Serial.printf("[admin] rx: %s\n", msg);
}
const char* cmdName(AdminCmd c) {
    switch (c) {
        case ADMIN_CMD_PARTY_START: return "party start";
        case ADMIN_CMD_PARTY_STOP:  return "party stop";
        case ADMIN_CMD_MESSAGE:     return "message";
        case ADMIN_CMD_LED:         return "led";
        case ADMIN_CMD_BEACON:      return "beacon";
        case ADMIN_CMD_STOP_ALL:    return "stop-all";
        default:                    return "?";
    }
}

// LVGL event callbacks (file scope; delegate to the g_admin singleton).
void adminBroadcastBtnCb(lv_event_t*) { g_admin.menuToggleBroadcast(); }
void adminCmdChangedCb(lv_event_t*)   { g_admin.menuCommandChanged(); }
void adminMenuLoadCb(lv_event_t*)     { g_admin.onMenuShown(); }
void adminMenuUnloadCb(lv_event_t*)   { g_admin.stopBroadcast(); }
void adminMsgboxDeleteCb(lv_event_t*) { g_admin.onMsgboxDeleted(); }

}  // namespace

// -----------------------------------------------------------------------------

uint8_t AdminService::messageCount() { return ADMIN_MESSAGE_COUNT; }

const char* AdminService::messageText(uint8_t idx) {
    return idx < ADMIN_MESSAGE_COUNT ? ADMIN_MESSAGES[idx] : "";
}

bool AdminService::secretIsDefault() const { return adminSecretIsDefault(); }

void AdminService::onEvent(const Event&) {}   // unused; driven by tick() + direct calls

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void AdminService::begin(EventBus& bus) {
    _bus = &bus;
    _loadUnlock();

    // Populate the on-device dropdowns from the single in-code sources so adding
    // a message or LED pattern automatically shows up here — no EEZ edit needed.
    if (objects.admin_message_dropdown) {
        String opts;
        for (uint8_t i = 0; i < ADMIN_MESSAGE_COUNT; i++) {
            if (i) opts += '\n';
            opts += ADMIN_MESSAGES[i];
        }
        lv_dropdown_set_options(objects.admin_message_dropdown, opts.c_str());
    }
    // LED registry, in enum order, so the selected index == LedPatternId.
    if (objects.admin_led_mode_dropdown) {
        String opts;
        ledPatternForEach([](LedPatternId, const char* name, void* user) {
            String* o = static_cast<String*>(user);
            if (o->length()) *o += '\n';
            *o += name;
        }, &opts);
        lv_dropdown_set_options(objects.admin_led_mode_dropdown, opts.c_str());
    }

    // Admin menu interactions (the main-menu card's navigation to this screen is
    // handled in EEZ Flow; C only reacts once the screen is shown).
    if (objects.admin_command_button)
        lv_obj_add_event_cb(objects.admin_command_button, adminBroadcastBtnCb,
                            LV_EVENT_CLICKED, nullptr);
    if (objects.admin_command_dropdown)
        lv_obj_add_event_cb(objects.admin_command_dropdown, adminCmdChangedCb,
                            LV_EVENT_VALUE_CHANGED, nullptr);
    if (objects.admin_menu) {
        // LOAD runs after EEZ's own screen-load handler (registered at boot), so
        // onMenuShown() can add the broadcast button to the rebuilt nav group.
        lv_obj_add_event_cb(objects.admin_menu, adminMenuLoadCb,
                            LV_EVENT_SCREEN_LOAD_START, nullptr);
        // Leaving the menu stops an in-flight continuous broadcast.
        lv_obj_add_event_cb(objects.admin_menu, adminMenuUnloadCb,
                            LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    }

    syncCardVisibility();   // reflect the NVS-restored unlock state at boot

    // Boot confirmation — proves the current firmware is running and shows this
    // device's role. A receiver must be unlocked=no; a controller unlocked=YES.
    Serial.printf("[admin] begin: unlocked=%s, dev-default-secret=%s\n",
                  _unlocked ? "YES" : "no", secretIsDefault() ? "YES" : "no");
}

void AdminService::tick() {
    // 1) Drain + authenticate received admin frames (queued on the NimBLE host
    //    task by ScanEngine). Verify + execute here on the main loop.
    AdminFrame f;
    while (g_scan_engine.popAdminFrame(f)) {
        // A device in admin (unlocked) mode is a controller: it broadcasts but
        // ignores admin commands received from other controllers — only
        // non-admin devices react. Still popped so the queue can't back up.
        if (_unlocked) { rxLog("ignored (this device is UNLOCKED — run 'admin lock')"); continue; }
        AdminCmd cmd;
        uint8_t  p1;
        uint16_t p2;
        if (!adminParseFrame(f.bytes, ADMIN_MSD_LEN, &cmd, &p1, &p2)) {
            rxLog("rejected (bad HMAC — different secret? — or malformed)");
            continue;
        }
        rxLog(cmdName(cmd));
        _execute(cmd, p1, p2);
    }

    const uint32_t now = millis();

    // 2) Broadcast-window expiry: every command advert is time-bounded, so
    //    stopBroadcast() runs when BROADCAST_MS elapses (unless stopped earlier).
    if (_bcasting && _bcast_until_ms && now >= _bcast_until_ms) stopBroadcast();

    // 3) Name-beacon expiry (receiver side; NimBLE self-stops the timed advert).
    if (_beacon_until_ms && now >= _beacon_until_ms) {
        _beacon_until_ms = 0;
        _advStop();
    }

    // 4) Message-box auto-dismiss (self-dismiss → no cooldown) + admin-LED deadline.
    if (_msg_until_ms && now >= _msg_until_ms) _closeMessage(/*auto_close=*/true);
    if (_led_until_ms && now >= _led_until_ms) _led_until_ms = 0;
}

// -----------------------------------------------------------------------------
// Unlock (send authorization)
// -----------------------------------------------------------------------------

bool AdminService::unlock(const char* password) {
    if (!adminCheckPassword(password)) return false;
    _unlocked = true;
    _persistUnlock();
    syncCardVisibility();
    g_display.refreshStatusIcons();   // show the admin icon (white)
    return true;
}

void AdminService::lock() {
    _unlocked = false;
    _persistUnlock();
    syncCardVisibility();
    if (_bcasting) stopBroadcast();   // no send authority once locked
    g_display.refreshStatusIcons();   // hide the admin icon
    // If we're sitting on the (now-unreachable) admin menu, pop back to the
    // previous screen (the menu was pushed on entry), matching the long-press
    // back gesture in UIController.
    if (lv_screen_active() == objects.admin_menu)
        eez_flow_pop_screen(LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
}

void AdminService::_loadUnlock() {
    Preferences p;
    if (p.begin(ADMIN_NVS_NS, /*readOnly=*/true)) {
        const bool     flag = p.getUChar(ADMIN_NVS_KEY, 0) != 0;
        const uint32_t fp   = p.getUInt(ADMIN_NVS_FP, 0);
        p.end();
        // Only honor a stored unlock if it was made under THIS build's secret —
        // reflashing firmware with a different secret (NVS survives) re-locks.
        _unlocked = flag && (fp == adminSecretFingerprint());
    }
}

void AdminService::_persistUnlock() {
    Preferences p;
    if (p.begin(ADMIN_NVS_NS, /*readOnly=*/false)) {
        p.putUChar(ADMIN_NVS_KEY, _unlocked ? 1 : 0);
        p.putUInt(ADMIN_NVS_FP, adminSecretFingerprint());
        p.end();
    }
}

// -----------------------------------------------------------------------------
// Send
// -----------------------------------------------------------------------------

bool AdminService::broadcast(AdminCmd cmd, uint8_t param1, uint16_t param2_secs) {
    if (!_unlocked) return false;
    uint8_t msd[ADMIN_MSD_LEN];
    const size_t n = adminBuildFrame(cmd, param1, param2_secs, msd);
    g_scan_engine.ensureBle();
    // A controller only transmits — no local execution (watch a non-admin device
    // to see the effect). Bounded broadcast window; re-run to extend.
    _startCommandAdvert(msd, n, BROADCAST_MS);
    _updateBroadcastButtonLabel();
    return true;
}

void AdminService::stopBroadcast() {
    if (!_bcasting) return;
    _bcasting       = false;
    _bcast_until_ms = 0;
    _advStop();
    g_leds.setBroadcast(false);              // drop the power-up; alert/ambient resume
    g_scan_engine.setScanInhibited(false);   // resume scanning
    _updateBroadcastButtonLabel();
    g_display.refreshStatusIcons();          // admin icon → white (unlocked, idle)
    Serial.println("[admin] TX: stopped");
}

bool AdminService::hasActiveEffect() const {
    return _msgbox != nullptr || _led_until_ms != 0 || _beacon_until_ms != 0;
}

uint16_t AdminService::_clampSecs(uint16_t secs) const {
    // FOREVER, over-cap, and 0 all collapse to the cap: bounds a replayed frame
    // and dodges the 0-means-"party default"/"LED forever" ambiguity.
    if (secs == 0 || secs == ADMIN_DUR_FOREVER || secs > ADMIN_MAX_EFFECT_SECS)
        return ADMIN_MAX_EFFECT_SECS;
    return secs;
}

void AdminService::_execute(AdminCmd cmd, uint8_t param1, uint16_t param2_secs) {
    const uint16_t secs   = _clampSecs(param2_secs);
    const uint32_t dur_ms = (uint32_t)secs * 1000UL;

    // Screen wake is per-command (below), NOT unconditional. Only MESSAGE and
    // PARTY_START draw something; posting EV_UI_ACTIVITY for every frame lit the
    // full display (backlight + LVGL render + main menu) for terminators
    // (party stop / stop-all) and screen-less effects (LED / beacon) too. Those
    // rebroadcast continuously, so a sleeping receiver re-woke to the main menu
    // on every timer-wake scan that caught one. LED / beacon still hold the
    // device awake to finish via _isInhibited()/hasActiveEffect() — screen dark.

    switch (cmd) {
        case ADMIN_CMD_PARTY_START:
            g_party.start(dur_ms, /*from_rule=*/false);   // authoritative; clears cooldown
            if (_bus) _bus->post(EV_UI_ACTIVITY);         // party takes over the overview screen
            break;

        case ADMIN_CMD_PARTY_STOP:
            g_party.stop(/*arm_cooldown=*/false);
            break;

        case ADMIN_CMD_MESSAGE:
            // Per-message cooldown: a manually-dismissed message is suppressed
            // for a while, but only that one — other messages still show.
            if (param1 < MSG_SLOTS && millis() < _msg_cooldown_until_ms[param1]) break;
            if (_msgbox && _msg_shown_idx == param1)
                _msg_until_ms = millis() + dur_ms;   // same message re-received — extend
            else
                _showMessage(param1, dur_ms);
            // Wake the panel + re-enable LVGL render so the modal box can draw.
            // Must follow _showMessage so it only fires when a box is actually up.
            if (_bus) _bus->post(EV_UI_ACTIVITY);
            break;

        case ADMIN_CMD_LED:
            if (param1 < LED_PATTERN_COUNT) {
                g_leds.playAlertPattern((LedPatternId)param1, dur_ms);
                _led_until_ms = millis() + dur_ms;
            }
            break;

        case ADMIN_CMD_BEACON:
            // Receiver only (controllers don't run commands locally), so the adv
            // slot is free — no burst to defer behind.
            _startNameBeacon(secs);
            break;

        case ADMIN_CMD_STOP_ALL:
            g_party.stop(/*arm_cooldown=*/false);
            g_leds.playAlertPattern(LED_PATTERN_OFF, 0);
            _led_until_ms = 0;
            _closeMessage(/*auto_close=*/true);   // global stop → no per-message cooldown
            if (_beacon_until_ms) { _advStop(); _beacon_until_ms = 0; }
            break;

        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// Advertising (single legacy slot; explicit stop-before-start)
// -----------------------------------------------------------------------------

// auto_stop_ms is the bounded advertise window (callers pass BROADCAST_MS); 0
// would mean advertise until stopBroadcast(), but no caller uses that now.
void AdminService::_startCommandAdvert(const uint8_t* msd, uint32_t len, uint32_t auto_stop_ms) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (!adv) { Serial.println("[admin] TX: getAdvertising() returned null"); return; }
    adv->stop();                       // start() no-ops while active — stop first
    NimBLEDevice::setPowerLevel(ADMIN_ADV_TX_LEVEL, ESP_BLE_PWR_TYPE_ADV);
    NimBLEAdvertisementData ad;
    ad.setManufacturerData(msd, len);
    adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    adv->enableScanResponse(false);   // non-scannable beacon — no scan-response packets
    adv->setMinInterval(ADV_ITVL_MIN);
    adv->setMaxInterval(ADV_ITVL_MAX);
    adv->setAdvertisementData(ad);
    const bool ok = adv->start(auto_stop_ms);
    // Logged unconditionally so a broadcast from EITHER the serial command or the
    // on-device menu button is visible over serial. msd[4]=cmd, msd[6..7]=secs.
    Serial.printf("[admin] TX %s (recv %us), window %lus, adv=%s\n",
                  cmdName((AdminCmd)msd[4]),
                  (unsigned)(msd[6] | ((uint16_t)msd[7] << 8)),
                  (unsigned long)(auto_stop_ms / 1000),
                  ok ? "STARTED" : "FAILED");
    _bcasting       = true;
    _bcast_until_ms = auto_stop_ms ? (millis() + auto_stop_ms) : 0;
    g_scan_engine.setScanInhibited(true);   // dedicate the radio to advertising
    g_leds.setBroadcast(true);              // yellow power-up while transmitting
    g_display.refreshStatusIcons();         // admin icon → yellow (broadcasting)
}

void AdminService::_startNameBeacon(uint16_t secs) {
    g_scan_engine.ensureBle();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (!adv) return;
    adv->stop();
    NimBLEDevice::setPowerLevel(ADMIN_ADV_TX_LEVEL, ESP_BLE_PWR_TYPE_ADV);  // see _startCommandAdvert

    char sfx[5];
    uint32_t r = esp_random();
    for (int i = 0; i < 4; i++) { sfx[i] = (char)('A' + (r % 26)); r /= 26; }
    sfx[4] = '\0';
    char name[24];
    snprintf(name, sizeof(name), "helgatchi %s", sfx);

    NimBLEAdvertisementData ad;
    ad.setName(name);
    adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);   // no GATT server — don't invite connects
    adv->enableScanResponse(false);   // non-scannable beacon — no scan-response packets
    adv->setMinInterval(ADV_ITVL_MIN);
    adv->setMaxInterval(ADV_ITVL_MAX);
    adv->setAdvertisementData(ad);

    const uint16_t s = _clampSecs(secs);
    if (!adv->start((uint32_t)s * 1000UL))
        Serial.println("[admin] ERROR: name-beacon advertising start failed");
    _beacon_until_ms = millis() + (uint32_t)s * 1000UL;
}

void AdminService::_advStop() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv) adv->stop();
}

// -----------------------------------------------------------------------------
// Message box (modal lv_msgbox, like the device-detail popup). Self-dismisses
// after `secs`; a long-press dismiss (handled generically in UIController, which
// deletes the msgbox backdrop) arms a per-message cooldown via onMsgboxDeleted.
// -----------------------------------------------------------------------------

void AdminService::_showMessage(uint8_t idx, uint32_t dur_ms) {
    const char* text = messageText(idx);
    if (!text || !*text) return;
    if (_msgbox) _closeMessage(/*auto_close=*/true);   // replace any open box (no cooldown)

    lv_obj_t* mb = lv_msgbox_create(nullptr);          // NULL parent → modal on the top layer
    lv_obj_set_width(mb, LV_PCT(85));
    lv_obj_set_style_radius(mb, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_msgbox_add_title(mb, "Helga says:");
    lv_obj_t* content = lv_msgbox_get_content(mb);
    if (content)
        lv_obj_set_style_text_font(content, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_msgbox_add_text(mb, text);

    // Suspend the underlying screen's keypad nav while modal (detach the keypad
    // group; long-press close still works — UIController scans the top layer
    // directly). Restored in onMsgboxDeleted.
    if (lv_indev_t* kbd = kbdIndev()) {
        _msg_saved_group = lv_indev_get_group(kbd);
        lv_indev_set_group(kbd, nullptr);
    }

    lv_obj_add_event_cb(mb, adminMsgboxDeleteCb, LV_EVENT_DELETE, nullptr);
    _msgbox         = mb;
    _msg_shown_idx  = idx;
    _msg_until_ms   = millis() + dur_ms;
    _msg_auto_close = false;
}

void AdminService::_closeMessage(bool auto_close) {
    if (!_msgbox) return;
    _msg_auto_close = auto_close;
    lv_obj_t* mb = (lv_obj_t*)_msgbox;
    lv_obj_t* backdrop = lv_obj_get_parent(mb);
    lv_obj_delete(backdrop ? backdrop : mb);   // fires adminMsgboxDeleteCb → onMsgboxDeleted
}

// Fires on ANY dismissal route (auto-timeout, replace, stop-all, or long-press).
void AdminService::onMsgboxDeleted() {
    if (lv_indev_t* kbd = kbdIndev())
        lv_indev_set_group(kbd, (lv_group_t*)_msg_saved_group);   // restore underlying nav
    _msg_saved_group = nullptr;
    // Only a manual (long-press) dismiss arms the per-message cooldown; C-driven
    // dismissals (timeout / replace / stop-all) set _msg_auto_close first.
    if (!_msg_auto_close && _msg_shown_idx < MSG_SLOTS)
        _msg_cooldown_until_ms[_msg_shown_idx] = millis() + PartyService::COOLDOWN_MS;
    _msgbox         = nullptr;
    _msg_until_ms   = 0;
    _msg_auto_close = false;
}

// -----------------------------------------------------------------------------
// On-device Admin Menu (§11)
// -----------------------------------------------------------------------------

void AdminService::syncCardVisibility() {
    setHidden(objects.admin_panel, !_unlocked);
}

void AdminService::onMenuShown() {
    // EEZ's SCREEN_LOAD_START handler (registered at boot, runs first) rebuilds
    // the nav group with only the dropdowns — add the broadcast button too.
    if (objects.admin_command_button)
        lv_group_add_obj(groups.UINavigation, objects.admin_command_button);
    _applyMenuVisibility();
    _updateBroadcastButtonLabel();
}

void AdminService::menuCommandChanged() {
    _applyMenuVisibility();
}

// Show the command container + button always; reveal only the extra containers
// the selected command needs. Command index == AdminCmd (dropdown order locked).
void AdminService::_applyMenuVisibility() {
    if (!objects.admin_command_dropdown) return;
    const AdminCmd cmd = (AdminCmd)lv_dropdown_get_selected(objects.admin_command_dropdown);
    const bool wants_msg = (cmd == ADMIN_CMD_MESSAGE);
    const bool wants_led = (cmd == ADMIN_CMD_LED);
    const bool wants_dur = (cmd == ADMIN_CMD_PARTY_START || cmd == ADMIN_CMD_MESSAGE ||
                            cmd == ADMIN_CMD_LED         || cmd == ADMIN_CMD_BEACON);
    setHidden(objects.admin_message_container,     !wants_msg);
    setHidden(objects.admin_led_pattern_container, !wants_led);
    setHidden(objects.admin_duration_container,    !wants_dur);
}

void AdminService::_updateBroadcastButtonLabel() {
    if (!objects.admin_command_button_label) return;
    lv_label_set_text(objects.admin_command_button_label,
                      _bcasting ? LV_SYMBOL_PAUSE " Stop broadcasting"
                                : LV_SYMBOL_PLAY  " Start broadcasting");
}

// Broadcast button toggle: pressed while idle starts broadcasting the selected
// command; pressed while broadcasting stops it (also stops on leaving the menu).
// Actions advertise continuously; terminators are a bounded burst. Never run
// locally — the device stays a controller on this screen.
void AdminService::menuToggleBroadcast() {
    if (!_unlocked) return;
    if (_bcasting) { stopBroadcast(); return; }
    if (!objects.admin_command_dropdown) return;

    const AdminCmd cmd = (AdminCmd)lv_dropdown_get_selected(objects.admin_command_dropdown);
    const uint16_t dur = durationFromIndex(
        objects.admin_duration_dropdown
            ? lv_dropdown_get_selected(objects.admin_duration_dropdown) : 2);
    uint8_t p1 = 0;
    if (cmd == ADMIN_CMD_MESSAGE && objects.admin_message_dropdown)
        p1 = (uint8_t)lv_dropdown_get_selected(objects.admin_message_dropdown);
    else if (cmd == ADMIN_CMD_LED && objects.admin_led_mode_dropdown)
        p1 = (uint8_t)lv_dropdown_get_selected(objects.admin_led_mode_dropdown);

    uint8_t msd[ADMIN_MSD_LEN];
    const size_t n = adminBuildFrame(cmd, p1, dur, msd);
    g_scan_engine.ensureBle();
    _startCommandAdvert(msd, n, BROADCAST_MS);
    _updateBroadcastButtonLabel();
}
