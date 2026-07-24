#pragma once
#include "event_bus.h"

// OverviewScreen
//
// Drives the Helga character animation on the overview screen. The EEZ design
// (create_screen_overview in src/UI/screens.c) builds a single lv_animimg,
// objects.helga, whose source is the whole 48-frame sprite sheet. This service
// owns *which* slice of that sheet plays and how — the raw widget and its
// frames stay in EEZ/screens.c; the behaviour lives here, mirroring how
// DevicesScreen/AlertsScreen split visuals (EEZ) from logic (C).
//
// Model (see overview_screen.cpp for the tables):
//   - A "clip" is a contiguous frame range with a per-frame duration and a
//     loop flag (index map + timing flags).
//   - A "HelgaAnim" is a composite: an optional intro clip (played once), a
//     sustained loop clip, and an optional outro clip (played once when
//     transitioning away). So play(HELGA_SNIFF) runs sniff_start once, loops
//     sniff_loop, and — when the next animation is requested — plays sniff_end
//     once to settle back before the new animation begins.
//
// Looping is implemented as play-once + re-arm in the anim completion callback
// (not LV_ANIM_REPEAT_INFINITE): LVGL's early_apply re-applies the first frame
// synchronously in the same tick, so the internal anim's terminal end-value
// frame never renders — a full-range loop would otherwise flash the frame
// after the range each cycle.
//
// Initialize AFTER g_ui.begin() (objects.* must exist).

enum HelgaAnim {
    HELGA_IDLE,     // default resting loop
    HELGA_SIT,      // sit / scoot loop
    HELGA_WALK,     // walk cycle loop
    HELGA_PARTY,    // party loop
    HELGA_DANCE,    // dance loop
    HELGA_SNIFF,    // sniff_start -> sniff_loop -> (exit) sniff_end
    HELGA_ALERT,    // sniff_start -> sniff_alert -> (exit) sniff_end
    HELGA_BRUSH,    // brushing loop
    HELGA__COUNT
};

class OverviewScreen : public IEventHandler {
public:
    void begin(EventBus& bus);
    void onEvent(const Event& e) override;

    // Request an animation. Always recorded as the desired state (even when the
    // overview isn't showing) so it's replayed on the next screen load; when the
    // overview IS showing it's driven live — if Helga is mid-animation the
    // current animation's outro and the new animation's intro (if any) are
    // sequenced first, then the new loop sustains.
    void play(HelgaAnim anim);

    // While held, bus events (scan/alert) no longer change the animation, so the
    // last play() sustains. Used by party mode to keep HELGA_PARTY on-screen for
    // its whole run. play() still works while held; releasing restores nothing —
    // the caller should play() the resume animation after hold(false).
    void hold(bool on);
};

extern OverviewScreen g_overview_screen;
