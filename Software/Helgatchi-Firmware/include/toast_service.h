#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Toast notifications
//
// One transient message at a time on the LVGL top layer, horizontally centered in
// the lower half of the screen (Android-style, so it doesn't cover whatever the
// operator was just looking at): it dwells long enough to read, fades out, and
// deletes itself. For feedback that doesn't deserve a screen or a modal — a press
// that can't go anywhere, an action that otherwise succeeds silently, a value that
// got clamped.
//
// Deliberately NOT an lv_msgbox. A msgbox builds a modal backdrop that dims the
// screen, swallows input, and is exactly what UIController's long-press "close
// any open msgbox" scan looks for. A toast is a plain non-clickable, non-
// focusable panel that belongs to no nav group, so it never touches focus,
// navigation, or button routing — the screen underneath keeps working while it's
// up, and it can't strand the operator by eating their next press.
//
// Visuals come from the EEZ-exported "Alert Card" style, so toasts follow the
// selected theme with the rest of the UI instead of inventing a look.
//
// No begin() — there's no state to initialize and nothing to subscribe to. It
// does require LVGL to be running, so only call it from UI code (i.e. after
// g_ui.begin()).
// ---------------------------------------------------------------------------

class ToastService {
public:
    static constexpr uint32_t DEFAULT_DWELL_MS = 1900;   // readable pause before the fade starts
    static constexpr uint32_t FADE_MS          = 180;

    // Show `text`, replacing any toast already on screen rather than stacking or
    // queueing. The string is copied into the label, so a stack buffer is fine.
    // `dwell_ms` is the pause before the fade begins, not the total lifetime.
    void show(const char* text, uint32_t dwell_ms = DEFAULT_DWELL_MS);

    // Start the fade now instead of waiting out the dwell. No-op if none is up.
    // Use before opening a modal that would otherwise sit under the toast.
    void dismiss();

    bool active() const;
};

extern ToastService g_toast;
