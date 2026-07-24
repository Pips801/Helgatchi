#include "alerts_screen.h"
#include "alerts_service.h"
#include "display_service.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "event_ids.h"
#include "event_payload.h"
#include "UI/screens.h"
#include "UI/styles.h"
#include "UI/eez-flow.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

AlertsScreen g_alerts_screen;

// ---------------------------------------------------------------------------
// Alerts list — dynamic cards rendered into objects.alert_container.
//
// Architecture: EEZ owns the visuals (Alert user widget design, styles,
// container layout). C owns the data flow — we build one LVGL card per
// AlertRecord whenever the alerts change. The card replicates the Alert
// user widget's tree structure (container + 2 labels + dismiss button)
// using EEZ-exported styles (`add_style_alert_card`,
// `add_style_focused___button`), so visuals match the design exactly.
//
// Dismiss flow mirrors EEZ's "Fade and Hide Alert" user action:
// `lv_anim_t` opacity 255→0 over 500 ms with EASE_OUT, then completion
// callback calls g_alerts.ack() which emits EV_ALERT_CLEARED. The cleared
// handler then deletes the card.
// ---------------------------------------------------------------------------

struct AlertCard {
    lv_obj_t* card         = nullptr;
    lv_obj_t* time_label   = nullptr;
    lv_obj_t* dismiss_btn  = nullptr;
    uint16_t  alert_id     = AlertsService::INVALID_ALERT;
    uint32_t  last_seen_ms = 0;            // cached for time-ago refresh
};

static AlertCard   _alert_cards[AlertsService::MAX_ALERTS];
static uint8_t     _alert_card_count        = 0;
static lv_timer_t* _alert_time_refresh_timer = nullptr;

static void _formatTimeAgo(char* buf, size_t buf_sz, uint32_t age_ms) {
    const uint32_t s = age_ms / 1000;
    if      (s < 60)   snprintf(buf, buf_sz, "%us ago",      (unsigned)s);
    else if (s < 3600) snprintf(buf, buf_sz, "%um %us ago",  (unsigned)(s / 60),   (unsigned)(s % 60));
    else               snprintf(buf, buf_sz, "%uh %um ago",  (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
}

static void _on_dismiss_anim_done(lv_anim_t* a) {
    auto* card = (lv_obj_t*)a->var;
    const uint16_t alert_id = (uint16_t)(uintptr_t)lv_obj_get_user_data(card);
    // Calling ack() emits EV_ALERT_CLEARED → AlertsScreen::onEvent →
    // _onAlertCleared() which deletes this widget. Don't lv_obj_del here.
    g_alerts.ack(alert_id);
}

static void _on_dismiss_click(lv_event_t* e) {
    auto* card = (lv_obj_t*)lv_event_get_user_data(e);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 500);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_completed_cb(&a, _on_dismiss_anim_done);
    lv_anim_start(&a);
}

// Builds one alert card mirroring the Alert user widget definition in EEZ.
// See create_user_widget_alert() in src/UI/screens.c for the source-of-truth
// hierarchy and styling.
static void _buildAlertCard(lv_obj_t* parent, const AlertRecord* rec, AlertCard* out) {
    lv_obj_t* card = lv_obj_create(parent);
    add_style_alert_card(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(card, (void*)(uintptr_t)rec->id);

    // Title label — top-left, Montserrat 16. Prefixed with a type icon
    // + single space so the source is identifiable at a glance. Skipped
    // entirely if the type has no symbol.
    const char* type_sym = nullptr;
    switch (rec->type) {
        case ALERT_BLE:          type_sym = LV_SYMBOL_BLUETOOTH;     break;
        case ALERT_WIFI:         type_sym = LV_SYMBOL_WIFI;          break;
        case ALERT_SYSTEM:       type_sym = LV_SYMBOL_BELL;          break;
        case ALERT_BATTERY_LOW:  type_sym = LV_SYMBOL_BATTERY_EMPTY; break;
        default:                 type_sym = nullptr;                 break;
    }
    char title_buf[sizeof(rec->title) + 8];
    if (type_sym && type_sym[0]) {
        snprintf(title_buf, sizeof(title_buf), "%s %s", type_sym, rec->title);
    } else {
        snprintf(title_buf, sizeof(title_buf), "%s", rec->title);
    }

    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_size(title, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(title, LV_ALIGN_TOP_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(title, title_buf);

    // Time-ago label — bottom-left, Montserrat 12, theme accent color [2].
    lv_obj_t* time_label = lv_label_create(card);
    lv_obj_set_size(time_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_align(time_label, LV_ALIGN_BOTTOM_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(time_label,
        lv_color_hex(theme_colors[eez_flow_get_selected_theme_index()][2]),
        LV_PART_MAIN | LV_STATE_DEFAULT);
    char buf[24];
    _formatTimeAgo(buf, sizeof(buf), millis() - rec->last_seen_ms);
    lv_label_set_text(time_label, buf);

    // Dismiss button — right side, with "Dismiss" label child.
    lv_obj_t* dismiss = lv_button_create(card);
    lv_obj_set_size(dismiss, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    add_style_focused___button(dismiss);
    lv_obj_add_event_cb(dismiss, _on_dismiss_click, LV_EVENT_CLICKED, card);
    lv_obj_t* dismiss_label = lv_label_create(dismiss);
    lv_label_set_text_static(dismiss_label, "Dismiss");
    lv_obj_set_style_align(dismiss_label, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    out->card         = card;
    out->time_label   = time_label;
    out->dismiss_btn  = dismiss;
    out->alert_id     = rec->id;
    out->last_seen_ms = rec->last_seen_ms;
}

static void _refreshTimeLabels() {
    const uint32_t now = millis();
    char buf[24];
    for (uint8_t i = 0; i < _alert_card_count; i++) {
        if (!_alert_cards[i].time_label) continue;
        _formatTimeAgo(buf, sizeof(buf), now - _alert_cards[i].last_seen_ms);
        lv_label_set_text(_alert_cards[i].time_label, buf);
    }
}

static void _alertTimeTimerCb(lv_timer_t* /*t*/) {
    _refreshTimeLabels();
}

static void _refreshNoAlertsLabel() {
    // Hidden when there are any alerts to show, visible when the list is
    // empty. The label itself is created and styled in EEZ Studio.
    const bool any = g_alerts.count() > 0;
    if (objects.no_alerts_label) {
        if (any) lv_obj_add_flag   (objects.no_alerts_label, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_remove_flag(objects.no_alerts_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// EV_ALERT_RAISED handler — inserts a new card at the TOP of the list.
// Newest alert visible first; older cards slide down.
static void _onAlertRaised(uint16_t alert_id) {
    if (!objects.alert_container) return;
    if (_alert_card_count >= AlertsService::MAX_ALERTS) return;
    const AlertRecord* rec = g_alerts.find(alert_id);
    if (!rec) return;

    // Shift the tracking array down so index 0 is the freshest card —
    // keeps _alert_cards[] aligned with on-screen render order.
    for (uint8_t i = _alert_card_count; i > 0; i--) {
        _alert_cards[i] = _alert_cards[i - 1];
    }
    AlertCard& slot = _alert_cards[0];
    slot = AlertCard{};
    _buildAlertCard(objects.alert_container, rec, &slot);
    _alert_card_count++;

    // lv_obj_create appends to the end of the parent's child list. Move to
    // index 0 so the flex layout places it at the top.
    lv_obj_move_to_index(slot.card, 0);

    // If the alerts screen is the active screen, sync the keypad nav group
    // to match visual order. LVGL 9 lv_group has no insert-at-index, so we
    // clear and re-add — cheap with ≤16 entries. The alerts screen has no
    // other widgets in this group (EEZ's screen-load handler clears it),
    // so wholesale clear is safe.
    if (lv_screen_active() == objects.alerts) {
        lv_group_remove_all_objs(groups.UINavigation);
        for (uint8_t i = 0; i < _alert_card_count; i++) {
            if (_alert_cards[i].dismiss_btn) {
                lv_group_add_obj(groups.UINavigation, _alert_cards[i].dismiss_btn);
            }
        }
    }
}

// EV_ALERT_UPDATED handler — dedup hit. Refresh the existing card's time
// label to "0s ago" immediately (rather than waiting up to a second for the
// next tick of the refresh timer).
static void _onAlertUpdated(uint16_t alert_id) {
    const AlertRecord* rec = g_alerts.find(alert_id);
    if (!rec) return;
    for (uint8_t i = 0; i < _alert_card_count; i++) {
        if (_alert_cards[i].alert_id != alert_id) continue;
        _alert_cards[i].last_seen_ms = rec->last_seen_ms;
        if (_alert_cards[i].time_label) {
            lv_label_set_text(_alert_cards[i].time_label, "0s ago");
        }
        return;
    }
}

// EV_ALERT_CLEARED handler — locate the card, delete it, compact the array.
// LVGL flex reflows the remaining cards automatically.
static void _onAlertCleared(uint16_t alert_id) {
    for (uint8_t i = 0; i < _alert_card_count; i++) {
        if (_alert_cards[i].alert_id != alert_id) continue;
        if (_alert_cards[i].card && lv_obj_is_valid(_alert_cards[i].card)) {
            lv_obj_del(_alert_cards[i].card);
        }
        for (uint8_t j = i; j + 1 < _alert_card_count; j++) {
            _alert_cards[j] = _alert_cards[j + 1];
        }
        _alert_card_count--;
        _alert_cards[_alert_card_count] = AlertCard{};
        return;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AlertsScreen::begin(EventBus& bus) {
    bus.subscribe(EV_ALERT_RAISED,  this);
    bus.subscribe(EV_ALERT_UPDATED, this);
    bus.subscribe(EV_ALERT_CLEARED, this);

    // Initial UI sync: empty-state label, status-bar bell, time-refresh timer.
    _refreshNoAlertsLabel();
    g_display.refreshStatusIcons();
    _alert_time_refresh_timer = lv_timer_create(_alertTimeTimerCb, 1000, nullptr);

    // Restore cards for any alerts that survived a deep-sleep wake (alerts
    // store persists in RTC slow memory). Iterate in reverse so the newest
    // ends up at index 0 — each _onAlertRaised shifts existing cards down.
    for (int i = (int)g_alerts.count() - 1; i >= 0; i--) {
        const AlertRecord* rec = g_alerts.get((uint8_t)i);
        if (rec) _onAlertRaised(rec->id);
    }

    // Repopulate keypad nav group with dismiss buttons when the alerts
    // screen loads (EEZ's own handler clears the group first).
    if (objects.alerts) {
        lv_obj_add_event_cb(objects.alerts, [](lv_event_t* /*e*/) {
            for (uint8_t i = 0; i < _alert_card_count; i++) {
                if (_alert_cards[i].dismiss_btn) {
                    lv_group_add_obj(groups.UINavigation, _alert_cards[i].dismiss_btn);
                }
            }
        }, LV_EVENT_SCREEN_LOAD_START, nullptr);
    }
}

void AlertsScreen::onEvent(const Event& e) {
    switch (e.id) {
        case EV_ALERT_RAISED: {
            _onAlertRaised(e.data.alert.alert_id);
            _refreshNoAlertsLabel();
            g_display.refreshStatusIcons();   // status-bar bell appears

            // SKEY_ALERT_FOCUS: jump to the alerts screen unless the user is
            // currently on settings (don't yank them mid-edit) or already on
            // the alerts screen. Once the focus-nav has fired for the current
            // alert batch, we stay put — back-navigation by the user is
            // respected. The latch clears when alert count returns to zero,
            // so a fresh batch later still pulls focus.
            if (!g_settings.getBool(SKEY_ALERT_FOCUS)) break;
            if (_focus_consumed) break;
            lv_obj_t* active = lv_screen_active();
            if (active == objects.settings) {
                // Don't disturb the operator mid-settings edit. Leave the
                // focus unconsumed so when they navigate away, the next
                // raised alert can still pull them.
                break;
            }
            if (active == objects.alerts) {
                // Already there — no navigation needed, but mark consumed so
                // the alert burst doesn't re-trigger any logic for the rest
                // of the batch.
                _focus_consumed = true;
                break;
            }
            // Push (not raw lv_screen_load) so the interrupted screen lands on
            // the EEZ page stack — long-press back returns the user to where
            // they were instead of stranding them (pop on an empty stack is a
            // no-op).
            eez_flow_push_screen(SCREEN_ID_ALERTS, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
            _focus_consumed = true;
            break;
        }

        case EV_ALERT_UPDATED:
            _onAlertUpdated(e.data.alert.alert_id);
            // Count didn't change → no need to refresh empty-state or bell.
            break;

        case EV_ALERT_CLEARED:
            _onAlertCleared(e.data.alert.alert_id);
            _refreshNoAlertsLabel();
            g_display.refreshStatusIcons();   // status-bar bell may disappear
            // When the list empties, allow focus-nav to fire again on the
            // next batch of incoming alerts.
            if (g_alerts.count() == 0) _focus_consumed = false;
            break;

        default:
            break;
    }
}
