#include "overview_screen.h"
#include "event_ids.h"
#include "UI/screens.h"
#include <lvgl.h>

OverviewScreen g_overview_screen;

// ---------------------------------------------------------------------------
// Frame map. Indices are into the 48-image array built in create_screen_overview
// (src/UI/screens.c) — keep in sync with that order if the sheet changes.
// ---------------------------------------------------------------------------

enum Clip {
    CLIP_SIT_SCOOT,
    CLIP_WALK,
    CLIP_PARTY,
    CLIP_DANCE,
    CLIP_SNIFF_ALERT,
    CLIP_SNIFF_END,
    CLIP_SNIFF_LOOP,
    CLIP_SNIFF_START,
    CLIP_BRUSH,
    CLIP_IDLE,
    CLIP_IDLE_FIDGET,
    CLIP_IDLE_SNEEZE,
    CLIP_IDLE_WAG,
    CLIP_IDLE_HEAD_TILT,
    CLIP_SLEEP,
    CLIP_SLEEP_START,
    CLIP_DANCE_TRANSITION,
    CLIP_DANCE_TRANS_OUT,
    CLIP__COUNT
};

// Per-frame duration, constant across all clips. A clip's total anim time is
// FRAME_MS * its frame count.
static constexpr uint16_t FRAME_MS = 250;

struct ClipDef {
    uint8_t first;   // first frame index (inclusive)
    uint8_t count;   // number of frames in the range
};

// first, count
static const ClipDef CLIPS[CLIP__COUNT] = {
    /* CLIP_SIT_SCOOT       */ {  0, 2 },
    /* CLIP_WALK            */ {  2, 4 },
    /* CLIP_PARTY           */ {  6, 8 },
    /* CLIP_DANCE           */ { 14, 8 },
    /* CLIP_SNIFF_ALERT     */ { 22, 4 },
    /* CLIP_SNIFF_END       */ { 26, 3 },
    /* CLIP_SNIFF_LOOP      */ { 29, 4 },
    /* CLIP_SNIFF_START     */ { 33, 2 },
    /* CLIP_BRUSH           */ { 35, 5 },
    /* CLIP_IDLE            */ { 40, 8 },
    /* CLIP_IDLE_FIDGET     */ { 48, 8 },
    /* CLIP_IDLE_SNEEZE     */ { 56, 8 },
    /* CLIP_IDLE_WAG        */ { 64, 8 },
    /* CLIP_IDLE_HEAD_TILT  */ { 72, 8 },
    /* CLIP_SLEEP           */ { 80, 6 },
    /* CLIP_SLEEP_START     */ { 86, 3 },
    /* CLIP_DANCE_TRANSITION*/ { 89, 2 },
    /* CLIP_DANCE_TRANS_OUT */ { 91, 2 },
};

// Composite animations: an optional intro (once), the sustained loop, and an
// optional outro (once, played when transitioning to another animation).
// -1 = none. Edit this table to re-map behaviour; no code changes needed.
struct AnimDef {
    int8_t intro;
    int8_t loop;
    int8_t outro;
};

static const AnimDef ANIMS[HELGA__COUNT] = {
    /* HELGA_IDLE  */ { -1,                     CLIP_IDLE,          -1                  },
    /* HELGA_IDLE2 */ { -1,                     CLIP_IDLE_FIDGET,   -1                  },
    /* HELGA_IDLE3 */ { -1,                     CLIP_IDLE_SNEEZE,   -1                  },
    /* HELGA_IDLE4 */ { -1,                     CLIP_IDLE_WAG,      -1                  },
    /* HELGA_IDLE5 */ { -1,                     CLIP_IDLE_HEAD_TILT,-1                  },
    /* HELGA_SIT   */ { -1,                     CLIP_SIT_SCOOT,     -1                  },
    /* HELGA_WALK  */ { -1,                     CLIP_WALK,          -1                  },
    /* HELGA_PARTY */ { CLIP_DANCE_TRANSITION,  CLIP_PARTY,         CLIP_DANCE_TRANS_OUT},
    /* HELGA_DANCE */ { CLIP_DANCE_TRANSITION,  CLIP_DANCE,         CLIP_DANCE_TRANS_OUT},
    /* HELGA_SNIFF */ { CLIP_SNIFF_START,       CLIP_SNIFF_LOOP,    CLIP_SNIFF_END      },
    /* HELGA_ALERT */ { -1,                     CLIP_SNIFF_ALERT,   -1                  },
    /* HELGA_BRUSH */ { -1,                     CLIP_BRUSH,         -1                  },
    /* HELGA_SLEEP */ { CLIP_SLEEP_START,       CLIP_SLEEP,         -1                  },
};

// ---------------------------------------------------------------------------
// Sequencer state
//
// A request builds a short queue of clips [old.outro?, new.intro?, new.loop].
// Each clip plays once (repeat_cnt = 1); the completion callback pops the next
// queued clip, or — when the queue is empty — re-arms the last clip, which is
// always the target's loop. So the terminal loop sustains and the transition
// clips play exactly once, in order.
// ---------------------------------------------------------------------------

static constexpr int QMAX = 4;
static int8_t  _queue[QMAX];
static uint8_t _qhead   = 0;   // index of next queued clip
static uint8_t _qlen    = 0;   // queued clips remaining
static int8_t  _current = -1;  // clip currently playing
static int8_t  _active  = -1;  // HelgaAnim whose loop is sustained (for outro lookup)
static int8_t  _target  = -1;  // HelgaAnim we're transitioning to
static bool    _running = false;

static void _startClip(int8_t clip) {
    const ClipDef& c = CLIPS[clip];
    _current = clip;
    _running = true;

    lv_anim_t* a = lv_animimg_get_anim(objects.helga);
    // Sweep [first, first+count): the exec cb maps the animated value directly
    // to a frame index, so count units == count frames each shown for frame_ms.
    lv_anim_set_values(a, c.first, c.first + c.count);
    lv_anim_set_duration(a, (uint32_t)FRAME_MS * c.count);
    lv_anim_set_repeat_count(a, 1);   // loop via re-arm in _completedCb, not INFINITE
    lv_animimg_start(objects.helga);
}

static void _startNext() {
    int8_t clip = _queue[_qhead++];
    _qlen--;
    if (_qlen == 0) _active = _target;   // the last queued clip is the target's loop
    _startClip(clip);
}

// Fires when a clip's single play finishes (in the LVGL anim timer). Advancing
// here — rather than with INFINITE repeat — keeps looping glitch-free: LVGL's
// early_apply re-applies the first frame synchronously before the next render,
// so the internal anim's terminal end-value frame is never displayed.
static void _completedCb(lv_anim_t* /*a*/) {
    if (_qlen > 0) _startNext();
    else           _startClip(_current);   // re-arm the sustained loop
}

static void _request(HelgaAnim next) {
    if (next == _target) return;   // already there / heading there

    _qhead = 0;
    _qlen  = 0;
    if (_active >= 0 && ANIMS[_active].outro >= 0) _queue[_qlen++] = ANIMS[_active].outro;
    if (ANIMS[next].intro >= 0)                    _queue[_qlen++] = ANIMS[next].intro;
    _queue[_qlen++] = ANIMS[next].loop;
    _target = next;

    // If idle (nothing running) kick it off now; otherwise the in-flight clip's
    // completion picks up the new queue at the next cycle boundary (no stutter).
    if (!_running) _startNext();
}

// ---------------------------------------------------------------------------
// Screen lifecycle
// ---------------------------------------------------------------------------

static void _enterOverview() {
    // Reset the sequencer and settle on the idle loop. _target/_active start
    // unset (-1) so the idle request isn't short-circuited.
    _qhead = _qlen = 0;
    _current = _active = _target = -1;
    _running = false;
    lv_animimg_set_completed_cb(objects.helga, _completedCb);
    _request(HELGA_IDLE);
}

static void _leaveOverview() {
    // Stop the frame anim so nothing animates off-screen. lv_anim_delete doesn't
    // fire the completion cb, so the sequencer stays put; the next load resets it.
    lv_animimg_delete(objects.helga);
    _running = false;
}

static void _screenEventCb(lv_event_t* e) {
    switch (lv_event_get_code(e)) {
        case LV_EVENT_SCREEN_LOAD_START:   _enterOverview(); break;
        case LV_EVENT_SCREEN_UNLOAD_START: _leaveOverview(); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Public API + lifecycle
// ---------------------------------------------------------------------------

void OverviewScreen::play(HelgaAnim anim) {
    if (!objects.helga) return;
    if (lv_screen_active() != objects.overview) return;   // only animate when visible
    _request(anim);
}

void OverviewScreen::begin(EventBus& bus) {
    // Helga sniffs while a scan window is open, reacts with the alert animation
    // when a rule fires, and settles to idle when the window closes. These are
    // the signals that actually fire today (PowerManager posts CMD_SCAN_START/
    // STOP per window); switch to EV_SCAN_STATE_CHANGED once ScanEngine emits it.
    bus.subscribe(CMD_SCAN_START,  this);
    bus.subscribe(CMD_SCAN_STOP,   this);
    bus.subscribe(EV_ALERT_RAISED, this);

    // Pixel-art scaling, driven in code because EEZ can't express it on an
    // animimg. Scale the *image* (lv_image_set_scale), not the widget transform:
    // the object-transform path composites through a layer smoothed by the
    // display's global antialiasing (lv_refr.c) — which lv_image_set_antialias
    // can't reach — whereas the image-scale path honours img->antialias, giving
    // crisp nearest-neighbour pixels. Requires the EEZ widget's transform scale
    // to be 256 (1x) so the two don't compound. The image's default align and
    // pivot are both CENTER, so the 4x frame stays centred in the 192x192 widget
    // with no pivot to manage; offsets start at 0 as a nudge knob.
    if (objects.helga) {
        lv_image_set_antialias(objects.helga, false);
        lv_image_set_scale(objects.helga, 768);   // 4x: 48px frame -> 192px
        // lv_image_set_offset_x(objects.helga, 0);
        // lv_image_set_offset_y(objects.helga, 0);
    }

    if (objects.overview) {
        lv_obj_add_event_cb(objects.overview, _screenEventCb, LV_EVENT_SCREEN_LOAD_START,   nullptr);
        lv_obj_add_event_cb(objects.overview, _screenEventCb, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
        // If the overview is already the active screen (shown during g_ui.begin,
        // before this cb was attached), take over the animation now.
        if (lv_screen_active() == objects.overview) _enterOverview();
    }
}

void OverviewScreen::onEvent(const Event& e) {
    switch (e.id) {
        case CMD_SCAN_START:  play(HELGA_SNIFF); break;
        case CMD_SCAN_STOP:   play(HELGA_IDLE);  break;
        case EV_ALERT_RAISED: play(HELGA_ALERT); break;
        default: break;
    }
}
