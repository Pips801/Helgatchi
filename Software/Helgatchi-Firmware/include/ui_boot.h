#pragma once
#include "event_bus.h"

// ---------------------------------------------------------------------------
// Deferred UI bring-up
//
// A TIMER wake is an autonomous scan cycle: deep sleep resets the chip, setup()
// reruns end to end, and the screen stays off for the whole window. Building
// LVGL, the EEZ screen tree and every screen service on that path is pure cost
// — nobody sees a pixel of it. Those steps are hoisted out of setup() into
// uiBringUpNow() and run only once something actually needs the panel.
//
// Who asks for it: PowerManager::_setDisplay on any ON/DIM transition. Every
// path that lights the panel funnels through there — cold boot, button wake,
// an alert with SKEY_ALERT_WAKE_SCREEN, serial power commands — so that single
// hook covers the normal cases. PartyService and the web-flash "updating"
// screen ask directly, since both can fire mid-window.
//
// Requests are DEFERRED, not serviced in place: uiRequestBringUp() can be
// called from inside g_bus.dispatch() (an alert waking the screen), and the
// screen begin()s subscribe to the bus — which EventBus documents as
// setup()-only and dispatch() iterates live. loop() services the request at the
// top of the iteration, outside dispatch.
//
// What makes this safe while the UI is down:
//   • Every objects.* is null. `objects` is a zero-init global and EEZ's
//     create_screens() never ran, so the existing `if (objects.x)` guards
//     scattered through the screen code already no-op.
//   • No screen service is subscribed, because none of them ran begin().
//     Nothing UI-driven can execute off the bus.
//   • The remaining hazards are the handful of non-UI services that call into
//     UI code (DisplayService status icons, LogService's LVGL counters,
//     PartyService's visuals). Those guard on uiIsUp() at the CALLEE, so
//     callers don't have to know.
// ---------------------------------------------------------------------------

// True once the full UI stack (LVGL + EEZ screens + screen services) is built.
bool uiIsUp();

// Ask for the UI. Idempotent and safe from any main-loop context, including
// mid-dispatch. No-op once the UI is up.
void uiRequestBringUp();

// True when a request is outstanding and not yet serviced.
bool uiBringUpPending();

// Build the UI stack, in the order setup() used to. Call ONLY from setup() or
// from loop() outside g_bus.dispatch() — it subscribes handlers to the bus.
// No-op when already up, or when nothing has requested it.
void uiBringUpNow(EventBus& bus);
