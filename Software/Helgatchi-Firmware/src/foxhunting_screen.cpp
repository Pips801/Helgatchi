#include "foxhunting_screen.h"
#include "scan_service.h"
#include "scan_engine.h"
#include "scan_types.h"
#include "vendor_lookup.h"
#include "led_service.h"        // hunt proximity meter (LED pulse + synced vibe tick)
#include "display_service.h"    // hunt status-bar icons (GPS + hunted radio)
#include "event_payload.h"
#include "UI/screens.h"
#include "UI/eez-flow.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

FoxhuntingScreen g_foxhunting_screen;

// RSSI (dBm) → 0..100 signal-quality bar. Clamped linear over -100..-30 dBm:
// -30 (right on top of it) pins full, -100 (far / through walls) bottoms out.
static constexpr int RSSI_Q_LO = -100;   // 0%
static constexpr int RSSI_Q_HI = -30;    // 100%

static lv_timer_t* _poll_timer = nullptr;
static constexpr uint32_t POLL_MS = 150;   // live refresh cadence

static int _rssiToQuality(int8_t rssi) {
    if (rssi <= RSSI_Q_LO) return 0;
    if (rssi >= RSSI_Q_HI) return 100;
    return (int)((long)(rssi - RSSI_Q_LO) * 100 / (RSSI_Q_HI - RSSI_Q_LO));
}

// One flat fill colour blended mathematically along red→yellow→green as quality
// (0..100) rises: red at 0, through orange/yellow/chartreuse, to green at 100.
// Full-saturation path (one channel always 255, the other ramps) so the mixed
// colours stay vivid — same hue sweep as the LEDs, red=far, green=close.
static lv_color_t _qualityColor(int q) {
    if (q < 0)   q = 0;
    if (q > 100) q = 100;
    uint8_t r, g;
    if (q < 50) { r = 255;                             g = (uint8_t)(q * 255 / 50);       }  // red→yellow
    else        { r = (uint8_t)((100 - q) * 255 / 50); g = 255;                            }  // yellow→green
    return lv_color_make(r, g, 0);
}

static void _formatTimeAgo(char* buf, size_t buf_sz, uint32_t age_ms) {
    const uint32_t s = age_ms / 1000;
    if      (s < 60)   snprintf(buf, buf_sz, "%us ago",     (unsigned)s);
    else if (s < 3600) snprintf(buf, buf_sz, "%um %us ago", (unsigned)(s / 60),   (unsigned)(s % 60));
    else               snprintf(buf, buf_sz, "%uh %um ago", (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
}

// set_text reallocs + re-measures even for identical text, so skip no-op writes
// — the poll runs several times a second.
static void _setLabelIfChanged(lv_obj_t* label, const char* text) {
    if (!label) return;
    if (strcmp(lv_label_get_text(label), text) == 0) return;
    lv_label_set_text(label, text);
}

void FoxhuntingScreen::_pollCb(lv_timer_t* /*t*/) {
    if (!g_foxhunting_screen._active) return;
    if (lv_screen_active() != objects.foxhunting_menu) return;   // not visible yet / already left
    g_foxhunting_screen._refresh();
}

void FoxhuntingScreen::begin(EventBus& bus) {
    _bus = &bus;

    // Any exit from the screen (long-press back-nav, or an alert forcing a
    // screen change) ends the hunt and resumes normal scanning.
    if (objects.foxhunting_menu) {
        lv_obj_add_event_cb(objects.foxhunting_menu, [](lv_event_t* /*e*/) {
            g_foxhunting_screen.stopHunt();
        }, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    }
}

void FoxhuntingScreen::onEvent(const Event& /*e*/) {}   // no subscriptions

void FoxhuntingScreen::startHunt(uint8_t domain, const uint8_t mac[6]) {
    const ScanResult* r = g_scan_service.findSeen(domain, mac);
    if (!r) return;   // aged out between selection and press — nothing to hunt

    _domain = domain;
    memcpy(_mac, mac, 6);
    _snap_rssi    = r->rssi;
    _snap_last_ms = r->timestamp_ms;

    // Static fields, captured once — name (or MAC when nameless) and the vendor
    // block. BLE MFG is the BT SIG company name (BLE + mfg id only); OUI MFG is
    // the IEEE org for the MAC (covers WiFi and BLE-without-mfg).
    char macbuf[24];
    snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (r->name[0]) { strncpy(_name, r->name, sizeof(_name) - 1); _name[sizeof(_name) - 1] = '\0'; }
    else            { strncpy(_name, macbuf,  sizeof(_name) - 1); _name[sizeof(_name) - 1] = '\0'; }

    const char* bt  = (domain == SCAN_BLE && r->mfg_id != 0) ? vendor_mfg_lookup(r->mfg_id) : nullptr;
    const char* oui = vendor_for_mac(mac);
    snprintf(_details, sizeof(_details),
             "MAC: %s\nOUI MFG: %s\nBLE MFG: %s",
             macbuf, oui ? oui : "-", bt ? bt : "-");

    // Paint the initial state before the screen shows.
    _setLabelIfChanged(objects.device_name,    _name);
    _setLabelIfChanged(objects.device_details, _details);
    _disp_q = _rssiToQuality(_snap_rssi);
    _active = true;
    g_leds.setHunt(true);              // arm the LED proximity meter + synced haptic ticks
    g_display.setHunt(true, domain);   // status bar → GPS + the hunted radio's icon
    _refresh();                        // paints labels/bar and pushes the first quality to the LEDs

    // Ask ScanEngine to lock onto this target (channel is only meaningful for
    // WiFi; ScanEngine ignores it for BLE).
    EventPayload p{};
    p.lockon.domain  = domain;
    memcpy(p.lockon.mac, mac, 6);
    p.lockon.channel = r->channel;
    _bus->post(CMD_SCAN_LOCKON_START, p);

    eez_flow_push_screen(SCREEN_ID_FOXHUNTING_MENU, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);

    if (!_poll_timer) _poll_timer = lv_timer_create(_pollCb, POLL_MS, nullptr);
}

void FoxhuntingScreen::stopHunt() {
    if (!_active) return;
    _active = false;
    if (_poll_timer) { lv_timer_delete(_poll_timer); _poll_timer = nullptr; }
    g_leds.setHunt(false);      // ambient LEDs resume; haptic ticks stop
    g_display.setHunt(false, 0); // normal BT/WiFi status icons return
    _bus->post(CMD_SCAN_LOCKON_STOP);
}

void FoxhuntingScreen::_refresh() {
    // Live target data from ScanEngine once it has a sighting; the start snapshot
    // until then. When the target goes quiet the RSSI holds its last value and
    // the "last seen" age just keeps counting up.
    int8_t   rssi;
    uint32_t last_ms;
    if (g_scan_engine.lockonHasHit()) {
        rssi    = g_scan_engine.lockonRssi();
        last_ms = g_scan_engine.lockonLastSeenMs();
    } else {
        rssi    = _snap_rssi;
        last_ms = _snap_last_ms;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "RSSI: %ddBm", (int)rssi);
    _setLabelIfChanged(objects.device_rssi, buf);

    char ago[24];
    _formatTimeAgo(ago, sizeof(ago), millis() - last_ms);
    snprintf(buf, sizeof(buf), "Last seen: %s", ago);
    _setLabelIfChanged(objects.last_seen, buf);

    // Light EMA over the RSSI-derived quality — smooths sample noise without the
    // 25 fps re-render that made the bar jitter.
    const int q = _rssiToQuality(rssi);
    _disp_q = (_disp_q * 3 + q + 2) / 4;
    if (objects.signal_quality) {
        lv_bar_set_value(objects.signal_quality, _disp_q, LV_ANIM_OFF);
        // Fill colour: one flat red→yellow→green colour by proximity (the bar's
        // fill is LV_PART_INDICATOR).
        lv_obj_set_style_bg_color(objects.signal_quality, _qualityColor(_disp_q),
                                  LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }
    g_leds.setHuntQuality((uint8_t)_disp_q);
}
