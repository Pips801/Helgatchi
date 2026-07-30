#include "alerts_screen.h"
#include "alerts_service.h"
#include "devices_screen.h"
#include "toast_service.h"
#include "display_service.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "event_ids.h"
#include "event_payload.h"
#include "UI/screens.h"
#include "UI/eez-flow.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

AlertsScreen g_alerts_screen;

// ---------------------------------------------------------------------------
// Alerts list — dynamic cards rendered into objects.alert_container.
//
// Architecture: EEZ owns the visuals (Alert user widget design, styles,
// container layout). C owns the data flow — we instantiate one copy of the
// generated Alert user widget per AlertRecord whenever the alerts change, so
// sizes, fonts, long-modes and styles are never re-declared here. Editing the
// widget in EEZ Studio is the only way to change how a card looks.
//
// Two focus stops per alert, in visual order: the card itself, then its
// dismiss button. Pressing the card opens the device-detail overlay for the
// device that fired the alert (AlertRecord carries the MAC + domain); pressing
// dismiss acks it. Only the card carries LV_OBJ_FLAG_EVENT_BUBBLE, so the
// button's CLICKED never reaches the card's handler.
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

// Child indices within one instantiated Alert widget. Creation order in
// create_user_widget_alert() and stable: title, time-ago, dismiss button. Only
// the two labels get `objects` slots, so the button is reached this way.
static constexpr uint32_t ALERT_CHILD_TITLE   = 0;
static constexpr uint32_t ALERT_CHILD_DISMISS = 2;

static void _formatTimeAgo(char* buf, size_t buf_sz, uint32_t age_ms) {
    const uint32_t s = age_ms / 1000;
    if      (s < 60)   snprintf(buf, buf_sz, "%us ago",      (unsigned)s);
    else if (s < 3600) snprintf(buf, buf_sz, "%um %us ago",  (unsigned)(s / 60),   (unsigned)(s % 60));
    else               snprintf(buf, buf_sz, "%uh %um ago",  (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
}

// Dismiss-fade exec callback.
//
// lv_anim_exec_xcb_t is void(*)(void*, int32_t), but lv_obj_set_style_opa takes a
// third `selector` argument — casting the function pointer to the anim signature
// (as this did) leaves that argument as whatever happens to be in the register,
// so the opacity lands on an arbitrary part/state combination and the fade is
// invisible. It only ever appeared to work when the junk value happened to be 0.
// Pass the selector explicitly.
static void _setCardOpa(void* card, int32_t opa) {
    lv_obj_set_style_opa((lv_obj_t*)card, (lv_opa_t)opa, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void _on_dismiss_anim_done(lv_anim_t* a) {
    auto* card = (lv_obj_t*)a->var;
    const uint16_t alert_id = (uint16_t)(uintptr_t)lv_obj_get_user_data(card);
    // Calling ack() emits EV_ALERT_CLEARED → AlertsScreen::onEvent →
    // _onAlertCleared() which deletes this widget. Don't lv_obj_del here.
    g_alerts.ack(alert_id);
}

// Pressing the card body (not the dismiss button) opens the device-detail
// overlay for the device this alert came from — the same popup the devices list
// opens, reached through the MAC + domain AlertsService records at raise time.
//
// Nothing to show for an alert with no originating sighting (battery, system,
// serial test alerts) or for a device that has since aged out of the scan
// service's seen map. Both dead ends raise a self-dismissing toast so the press
// reads as "nothing to show" rather than a broken selection.
static void _on_card_click(lv_event_t* e) {
    auto* card = (lv_obj_t*)lv_event_get_user_data(e);
    // Act only on the card's own press, never a bubbled child's — dismissing an
    // alert must not also open the device overlay. Belt-and-braces: _buildAlertCard
    // strips the button's EVENT_BUBBLE, so nothing reaches here today.
    if (lv_event_get_target(e) != card) return;
    const uint16_t alert_id = (uint16_t)(uintptr_t)lv_obj_get_user_data(card);
    const AlertRecord* rec = g_alerts.find(alert_id);
    if (!rec) return;
    if (!rec->has_device) {
        Serial.printf("[alerts] alert %u has no device to show\n", (unsigned)alert_id);
        g_toast.show("No device for this alert");
        return;
    }
    if (!g_devices_screen.openDeviceDetail(rec->domain, rec->mac, rec->title)) {
        Serial.printf("[alerts] %02X:%02X:%02X:%02X:%02X:%02X no longer in the seen map\n",
                      rec->mac[0], rec->mac[1], rec->mac[2],
                      rec->mac[3], rec->mac[4], rec->mac[5]);
        g_toast.show("Device no longer seen");
    }
}

// Long-title handling: DOTS at rest, SCROLL_CIRCULAR while the card is selected.
// The marquee is a continuous animation that invalidates its label every tick —
// fine on the one focused card, a standing tax if every card ran one. LVGL only
// starts the anim when the text actually overflows the label's fixed width (which
// comes from the EEZ widget), so short titles cost nothing even while focused.
//
// set_long_mode has no same-value early-out (it kills and restarts the anim and
// re-measures), so guard it — otherwise a repeat event would restart the scroll
// from the beginning.
static void _setLongModeIfChanged(lv_obj_t* label, lv_label_long_mode_t mode) {
    if (!label) return;   // child lookup depends on the EEZ widget's structure
    if (lv_label_get_long_mode(label) == mode) return;
    lv_label_set_long_mode(label, mode);
}

// The marquee follows the selected ALERT, not the selected widget: a card and its
// dismiss button are two focus stops within one alert, so stepping between them
// must not interrupt the scroll — only moving to a different alert does.
//
// Driven off LV_EVENT_FOCUSED only, never DEFOCUSED: lv_group sends DEFOCUSED
// *before* it moves obj_focus (see focus_next_core), so a defocus can't tell
// where focus is heading. Instead each focus asserts "this alert scrolls", and
// the previous one is reverted here. A card→dismiss step re-asserts the same
// card, _setLongModeIfChanged early-outs, and the running animation is left
// alone — which is the whole point.
static lv_obj_t* _marquee_card = nullptr;   // card whose title is scrolling, or null

static void _setMarqueeCard(lv_obj_t* card) {
    if (_marquee_card == card) return;
    // Never dangles: _onAlertCleared clears this before deleting a card, so the
    // refocus that deletion triggers can't land back on a freed object.
    if (_marquee_card) {
        _setLongModeIfChanged(lv_obj_get_child(_marquee_card, ALERT_CHILD_TITLE),
                              LV_LABEL_LONG_MODE_DOTS);
    }
    _marquee_card = card;
    if (card) {
        _setLongModeIfChanged(lv_obj_get_child(card, ALERT_CHILD_TITLE),
                              LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    }
}

// Registered on both the card and its dismiss button, each with the CARD as
// user_data — that's what makes the two stops resolve to the same alert.
static void _on_alert_focus(lv_event_t* e) {
    _setMarqueeCard((lv_obj_t*)lv_event_get_user_data(e));
}

static void _on_dismiss_click(lv_event_t* e) {
    auto* card = (lv_obj_t*)lv_event_get_user_data(e);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, card);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 500);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, _setCardOpa);
    lv_anim_set_completed_cb(&a, _on_dismiss_anim_done);
    lv_anim_start(&a);
}

// Instantiate the EEZ-designed Alert user widget, so its layout, sizes, fonts,
// long-modes, and styles come straight from create_user_widget_alert() in
// src/UI/screens.c — edit the widget in EEZ Studio and it lands here on the next
// export. Only data and behaviour are C's business below.
//
// Same slot-lending trick the device pool uses (see _buildPoolCard): the
// generated builder writes its child pointers into objects[startWidgetIndex + N],
// but this widget isn't placed on a screen (so it owns no reserved slots) and is
// never ticked by EEZ, so we lend it slots [0..2] for the call, read the created
// objects back, then restore the originals. Index mapping matches the
// assignments in create_user_widget_alert(): +0 card, +1 time label, +2 title.
// The dismiss button gets no slot, so it's fetched by child index — creation
// order in the generated builder is [title, time, dismiss], and stable.
static void _buildAlertCard(lv_obj_t* parent, const AlertRecord* rec, AlertCard* out) {
    lv_obj_t** slots = (lv_obj_t**)&objects;
    lv_obj_t*  saved[3];
    for (int i = 0; i < 3; i++) saved[i] = slots[i];

    create_user_widget_alert(parent, nullptr, 0);

    lv_obj_t* card       = slots[0];
    lv_obj_t* time_label = slots[1];
    lv_obj_t* title      = slots[2];

    for (int i = 0; i < 3; i++) slots[i] = saved[i];

    lv_obj_t* dismiss = lv_obj_get_child(card, ALERT_CHILD_DISMISS);

    // The card and the dismiss button are two independent focus stops, so only
    // one of them may wear the FOCUS_KEY border at a time. EEZ gives the button
    // LV_OBJ_FLAG_EVENT_BUBBLE, which breaks that: lv_obj_event's base handler
    // acts on lv_event_get_current_target, so a bubbled LV_EVENT_FOCUSED runs
    // again with the card as the target and adds LV_STATE_FOCUS_KEY to it too —
    // both light up. Stripping the flag also stops the button's CLICKED from
    // reaching the card's own handler. Untick EVENT_BUBBLE on the button in EEZ
    // Studio and this line can go.
    if (dismiss) lv_obj_remove_flag(dismiss, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Scrolls the alert CONTAINER so the focused card is on screen (nothing to do
    // with the title marquee below — that's the label's long mode). The card is a
    // keypad focus stop of its own, so it needs the same treatment
    // lv_button_create already gives the dismiss button; without it focus lands
    // on an off-screen card with no visible ring. Not a visual property, so it
    // stays here rather than in the EEZ widget flags.
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_user_data(card, (void*)(uintptr_t)rec->id);
    lv_obj_add_event_cb(card, _on_card_click, LV_EVENT_CLICKED, card);
    if (dismiss) lv_obj_add_event_cb(dismiss, _on_dismiss_click, LV_EVENT_CLICKED, card);

    // Dots → marquee while this ALERT is selected, whether the focus sits on the
    // card or on its dismiss button (see _setMarqueeCard). Both register the card
    // so the two stops resolve to the same alert. The widget's resting long mode
    // stays DOT in EEZ; this only overrides it for the duration of the focus.
    lv_obj_add_event_cb(card, _on_alert_focus, LV_EVENT_FOCUSED, card);
    if (dismiss) lv_obj_add_event_cb(dismiss, _on_alert_focus, LV_EVENT_FOCUSED, card);

    // Title text — the type icon + a single space prefixes the alert title so the
    // source is identifiable at a glance. Skipped entirely if the type has no
    // symbol. (EEZ's own binding is the equivalent Symbol + " " + AlertTitle
    // expression, evaluated by the flow engine; this widget isn't flow-ticked.)
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
    lv_label_set_text(title, title_buf);

    char buf[24];
    _formatTimeAgo(buf, sizeof(buf), millis() - rec->last_seen_ms);
    lv_label_set_text(time_label, buf);

    out->card         = card;
    out->time_label   = time_label;
    out->dismiss_btn  = dismiss;
    out->alert_id     = rec->id;
    out->last_seen_ms = rec->last_seen_ms;
}

// Populate the keypad nav group in visual order: each card followed by its own
// dismiss button, newest alert first. Callers that need the group rebuilt from
// scratch clear it first — LVGL 9's lv_group has no insert-at-index, and with
// ≤16 alerts (≤32 entries) a wholesale rebuild is cheap. The alerts screen has
// no other widgets in this group (EEZ's screen-load handler clears it), so a
// wholesale clear is safe.
static void _addCardsToNavGroup() {
    for (uint8_t i = 0; i < _alert_card_count; i++) {
        if (_alert_cards[i].card)        lv_group_add_obj(groups.UINavigation, _alert_cards[i].card);
        if (_alert_cards[i].dismiss_btn) lv_group_add_obj(groups.UINavigation, _alert_cards[i].dismiss_btn);
    }
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

    // If the alerts screen is the active screen, re-sync the keypad nav group so
    // its order matches the new visual order (the fresh card jumped to the top).
    if (lv_screen_active() == objects.alerts) {
        lv_group_remove_all_objs(groups.UINavigation);
        _addCardsToNavGroup();
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
        // Release the marquee BEFORE deleting: lv_obj_del removes the card from
        // the nav group, which refocuses onto a sibling and re-enters
        // _setMarqueeCard — it must not find this card still on the hook.
        if (_marquee_card == _alert_cards[i].card) _marquee_card = nullptr;
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

    // Repopulate the keypad nav group with this screen's cards + dismiss buttons
    // when the alerts screen loads (EEZ's own handler clears the group first).
    if (objects.alerts) {
        lv_obj_add_event_cb(objects.alerts, [](lv_event_t* /*e*/) {
            _addCardsToNavGroup();
        }, LV_EVENT_SCREEN_LOAD_START, nullptr);

        // Leaving the screen only produces a DEFOCUSED, which the marquee doesn't
        // listen to — stop it explicitly so no off-screen card keeps an animation
        // invalidating its label every tick. Re-entry re-focuses and restarts it.
        lv_obj_add_event_cb(objects.alerts, [](lv_event_t* /*e*/) {
            _setMarqueeCard(nullptr);
        }, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
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
