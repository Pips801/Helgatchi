#include "power_menu_screen.h"
#include "power_manager.h"
#include "event_ids.h"
#include "UI/screens.h"
#include "UI/eez-flow.h"   // eez_flow_set_screen
#include <lvgl.h>
#include <stdio.h>

PowerMenuScreen g_power_menu_screen;

static EventBus* _bus = nullptr;

// ---------------------------------------------------------------------------
// Power action → Power Action screen → (hold) → command on the bus
//
// A button doesn't fire its command immediately. It swaps to the Power Action
// screen (fade-in, so "Restarting now…" etc. is visible), then a one-shot
// timer posts the actual CMD_POWER_* after a short hold — a visual beat before
// the device tears down. Command post is deferred anyway (the bus drains next
// loop), so the message stays on screen through the transition.
// ---------------------------------------------------------------------------

static constexpr uint32_t POWER_ACTION_HOLD_MS = 1800;   // in the user's 1.5–2 s window

static lv_timer_t* _action_timer   = nullptr;
static EventId     _pending_action = CMD_POWER_SLEEP;    // set before the timer is armed

// Cancel a pending action — stop the timer, post nothing.
static void _cancelPowerAction() {
    if (_action_timer) { lv_timer_delete(_action_timer); _action_timer = nullptr; }
}

static void _action_timer_cb(lv_timer_t* /*t*/) {
    // Stop first: the command is queued (not synchronous), so without this the
    // repeating timer would re-fire the transition every hold interval.
    _cancelPowerAction();
    if (_bus) _bus->post(_pending_action);
    // Screen-off is the one action the device survives with LVGL intact — pop
    // the action screen now so the stale "Turning off screen..." isn't what
    // greets the user when the display wakes later.
    if (_pending_action == CMD_POWER_SCREEN_OFF) {
        eez_flow_pop_screen(LV_SCR_LOAD_ANIM_NONE, 0, 0);
    }
}

// Backing out of the Power Action screen (long-press → pop) cancels the pending
// action. The committed paths (reboot/sleep/off) tear the device down without
// an LVGL unload, so this fires only when the user bails or on the screen-off
// pop above (where the timer is already gone), never on the real go.
static void _on_action_unload(lv_event_t* /*e*/) {
    _cancelPowerAction();
}

// One message per power action, defined once — the menu buttons, the serial
// `power` subcommands, and the settings Reset-device button all converge here.
static const char* _actionMsg(EventId cmd) {
    switch (cmd) {
        case CMD_POWER_SLEEP:          return "Sleeping now...";
        case CMD_POWER_REBOOT:         return "Restarting now...";
        case CMD_POWER_DOWN:           return "Powering off...";
        case CMD_POWER_SHIPPING_RESET: return "Wiping for shipping...";
        case CMD_POWER_FACTORY_RESET:  return "Wiping device...";
        case CMD_POWER_SCREEN_OFF:     return "Turning off screen...";
        default:                       return nullptr;
    }
}

static void _beginPowerAction(EventId cmd) {
    if (_action_timer) return;   // an action is already counting down
    const char* msg = _actionMsg(cmd);
    if (!msg) return;            // not a power action — refuse rather than show a blank screen

    // Force the display on: with it off, rendering AND lv_timers are
    // suspended, so a serially-triggered action would neither show its
    // message nor ever fire its countdown.
    g_power.wakeScreen();

    _pending_action = cmd;
    lv_label_set_text_static(objects.power_action_text, msg);
    // push, not set: set_screen zeroes the EEZ page stack, which makes the
    // long-press back-nav's eez_flow_pop_screen a no-op — the action screen
    // would never unload and the cancel would never fire. push records the
    // current screen so pop returns to it and unloads this screen.
    eez_flow_push_screen(SCREEN_ID_POWER_ACTION_SCREEN, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0);
    _action_timer = lv_timer_create(_action_timer_cb, POWER_ACTION_HOLD_MS, nullptr);
}

void PowerMenuScreen::beginAction(EventId cmd) {
    _beginPowerAction(cmd);
}

// ---------------------------------------------------------------------------
// Button CLICKED callbacks — show the action screen, then hand off to
// PowerManager (peripheral teardown + the actual transition live there).
// ---------------------------------------------------------------------------

static void _on_sleep_now(lv_event_t* /*e*/) {
    _beginPowerAction(CMD_POWER_SLEEP);
}

static void _on_restart(lv_event_t* /*e*/) {
    _beginPowerAction(CMD_POWER_REBOOT);
}

static void _on_power_off(lv_event_t* /*e*/) {
    // Deep sleep, no timer — wakes only on a CENTER long-hold. Unlike shipping
    // it leaves the tutorial flag intact.
    _beginPowerAction(CMD_POWER_DOWN);
}

// ---------------------------------------------------------------------------
// Lifecycle — must follow g_ui.begin() so objects.* are valid.
// ---------------------------------------------------------------------------

void PowerMenuScreen::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(EV_SLEEP_COUNTDOWN_UPDATED, this);

    lv_obj_add_event_cb(objects.sleep_now_button, _on_sleep_now,  LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.restart_button,   _on_restart,    LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.power_off_,       _on_power_off,  LV_EVENT_CLICKED, nullptr);

    // Back out of the action screen → cancel whatever was counting down.
    lv_obj_add_event_cb(objects.power_action_screen, _on_action_unload,
                        LV_EVENT_SCREEN_UNLOAD_START, nullptr);
}

// ---------------------------------------------------------------------------
// IEventHandler — reflect the sleep countdown while this screen is active.
// Same 1 Hz EV_SLEEP_COUNTDOWN_UPDATED PowerManager posts for the Settings
// screen: 0xFFFF = inhibited ("will not auto-sleep"), else seconds remaining.
// ---------------------------------------------------------------------------

void PowerMenuScreen::onEvent(const Event& e) {
    if (e.id != EV_SLEEP_COUNTDOWN_UPDATED) return;
    if (lv_scr_act() != objects.power_menu) return;

    uint16_t s = e.data.sleep_count.seconds;
    if (s == 0xFFFFu) {
        lv_label_set_text_static(objects.sleep_countdown_text,
                                 "Sleep now (will not auto-sleep)");
    } else {
        char buf[40];
        snprintf(buf, sizeof(buf), "Sleep now (will sleep in %us)", s);
        lv_label_set_text(objects.sleep_countdown_text, buf);
    }
}
