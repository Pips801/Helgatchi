#pragma once
#include "event_bus.h"
#include <stdint.h>

// PartyService — the "party mode" device state.
//
// Party mode is a sustained, look-at-me state: a looping rainbow LED pattern,
// rhythmic haptic pulses, Helga's dance animation on the overview screen, and a
// colour-cycling "PARTY MODE" banner on the top bar. It runs for a fixed
// duration (re-triggering refreshes the timer) and any button press cancels it.
//
// Two entry points, both funnelling through start()/stop():
//   - the serial `party` command (SerialConsole::_cmdParty), and
//   - a RULE_ACTION_PARTY rule firing (RulesService::_fire calls start()).
//
// Navigation: start() always brings up the overview (status) page via
// eez_flow_set_screen. Exiting party — long-press (handled in UIController),
// serial `party off`, or timeout — only tears down the effects and STAYS on the
// status page; it never navigates. Backing out of the status page itself is
// UIController's job (long-press on the overview → main menu). tick() also ends
// the party if the screen changes away from the overview by any other route.
//
// The banner is a label this service owns and parents onto the overview top bar
// at runtime. The EEZ-bound title label's *text* is reasserted every flow tick
// (evalTextProperty in tick_user_widget_top_bar), so we can't repurpose it for
// custom text from C; we hide it and draw our own instead. Its *colour* would
// survive the tick, but cycling a colour per-frame is a C-side job either way —
// so owning the whole banner keeps it self-contained with zero .eez-project /
// generated-file changes.
//
// Initialize AFTER g_ui + g_overview_screen (it references objects.*).

class PartyService : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();                        // call every loop()
    void onEvent(const Event& e) override;

    // Start (or extend, if already active) party mode. from_rule=true means the
    // trigger was a RULE_ACTION_PARTY match; those are ignored during the
    // post-dismiss cooldown so a persistent party beacon can't instantly
    // re-trigger after the user backs out. An explicit (serial) start clears the
    // cooldown and always runs.
    void start(uint32_t duration_ms, bool from_rule = false);
    // End now + restore LEDs / title / idle anim. arm_cooldown=true (the default,
    // used by serial `party off` / long-press) locks out rule re-triggers for
    // COOLDOWN_MS; admin PARTY_STOP / STOP_ALL pass false so a rule party can
    // resume immediately on receivers.
    void stop(bool arm_cooldown = true);

    bool     active() const { return _active; }
    uint32_t remainingMs() const;

    static constexpr uint32_t DEFAULT_DURATION_MS = 20000;      // 20 s
    static constexpr uint32_t COOLDOWN_MS         = 5 * 60000;  // 5 min re-trigger lockout after dismiss

private:
    void _ensureBanner();               // lazily create the overlay banner label
    void _refreshColors();              // advance the hue; repaint banner + tint the status icons
    void _end(bool set_cooldown);       // tear down effects (no navigation); arm cooldown if asked

    EventBus* _bus              = nullptr;
    bool      _active           = false;
    bool      _settled          = false;   // overview has become the active screen at least once
    uint32_t  _until_ms         = 0;
    uint32_t  _cooldown_until_ms = 0;      // rule triggers ignored until this millis()
    uint32_t  _last_vibe_ms     = 0;
    uint32_t  _last_text_ms     = 0;
    uint32_t  _last_awake_ms    = 0;
    uint16_t  _hue              = 0;       // 0..359, current banner/icon hue
    void*     _banner           = nullptr; // lv_obj_t* (own label; void* to keep lvgl out of the header)
};

extern PartyService g_party;
