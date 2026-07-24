#include "overview_screen.h"
#include "event_ids.h"
#include "scan_types.h"
#include "party_service.h"
#include "UI/screens.h"
#include <lvgl.h>
#include <esp_random.h>
#include <cstdio>

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
static constexpr uint16_t FRAME_MS = 200;

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
static int8_t  _desired = HELGA_IDLE;  // last animation the bus/API asked for,
                                       // tracked even while the overview is closed
                                       // so opening it resumes the right state
static bool    _held    = false;       // when set, ignore bus-driven state changes
                                       // (scan/alert) so a caller-owned animation
                                       // (e.g. party mode) sustains — see hold()

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
// Status text ("what Helga is doing")
//
// A small state machine independent of the sprite: the sprite reacts to coarse
// CMD_SCAN_START/STOP, but the status line distinguishes BLE vs WiFi (from
// EV_SCAN_STATE_CHANGED) and adds low-battery / party lines. Precedence:
//   Party > Alert > Scanning > Low battery > Idle
// Wording is re-rolled from the phrase tables only when the state actually
// changes, so a routine battery tick doesn't reshuffle the text mid-state. The
// alert line latches on EV_ALERT_RAISED and holds until the next state-changing
// event replaces it (no timer). Off-screen we track state but write no LVGL;
// _enterOverview forces a fresh write on load.
// ---------------------------------------------------------------------------

enum StatusState {
    ST_INVALID = -1,
    ST_IDLE, ST_SCAN_BLE, ST_SCAN_WIFI, ST_ALERT, ST_LOW_BATT, ST_PARTY
};

static constexpr uint8_t LOW_BATTERY_PCT = 15;   // matches led_service.cpp

static bool    _ble_scanning  = false;
static bool    _wifi_scanning = false;
static uint8_t _batt_pct      = 100;   // 0..100 real; BATT_PCT_* sentinels (>100) never low
static bool    _alert_latched = false;
static int8_t  _status_state  = ST_INVALID;

static const char* const IDLE_WORDS[]  = { "bored", "chilling", "idle" };
static const char* const SNIFF_VERBS[] = { "sniffing", "snorting", "hoovering" };
static const char* const BLE_NOUNS[]   = { "BLE packets", "BLE advertisements", "BLE data" };
static const char* const WIFI_NOUNS[]  = { "WiFi frames", "WiFi packets", "WiFi data" };
static const char* const TIRED_WORDS[] = { "tired", "sleepy" };
static const char* const PARTY_WORDS[] = { "getting crunk", "getting turnt", "partying" };

static const char* _pick(const char* const* arr, size_t n) { return arr[esp_random() % n]; }
#define PICK(a) _pick((a), sizeof(a) / sizeof((a)[0]))

static bool _isLowBatt() { return _batt_pct <= 100 && _batt_pct < LOW_BATTERY_PCT; }

static StatusState _computeStatus() {
    if (g_party.active()) return ST_PARTY;
    if (_alert_latched)   return ST_ALERT;
    if (_wifi_scanning)   return ST_SCAN_WIFI;
    if (_ble_scanning)    return ST_SCAN_BLE;
    if (_isLowBatt())     return ST_LOW_BATT;
    return ST_IDLE;
}

// Recompute the status line. Only rewrites the label when the state changes
// (a re-enter forces it via _status_state = ST_INVALID). Off-screen: state is
// tracked, LVGL is untouched.
static void _updateStatusText() {
    StatusState s = _computeStatus();
    if (s == _status_state) return;
    _status_state = s;

    if (!objects.helga_status_text || lv_screen_active() != objects.overview) return;

    char buf[64];
    switch (s) {
        case ST_SCAN_BLE:
            snprintf(buf, sizeof(buf), "Helga is %s %s", PICK(SNIFF_VERBS), PICK(BLE_NOUNS));
            break;
        case ST_SCAN_WIFI:
            snprintf(buf, sizeof(buf), "Helga is %s %s", PICK(SNIFF_VERBS), PICK(WIFI_NOUNS));
            break;
        case ST_ALERT:
            snprintf(buf, sizeof(buf), "Helga found something!");
            break;
        case ST_LOW_BATT:
            snprintf(buf, sizeof(buf), "Helga is %s", PICK(TIRED_WORDS));
            break;
        case ST_PARTY:
            snprintf(buf, sizeof(buf), "Helga is %s", PICK(PARTY_WORDS));
            break;
        case ST_IDLE:
        default:
            snprintf(buf, sizeof(buf), "Helga is %s", PICK(IDLE_WORDS));
            break;
    }
    lv_label_set_text(objects.helga_status_text, buf);
}

// ---------------------------------------------------------------------------
// Screen lifecycle
// ---------------------------------------------------------------------------

static void _enterOverview() {
    // Reset the sequencer and settle on whatever state the bus events left us in
    // while the screen was closed (idle if nothing fired). _target/_active start
    // unset (-1) so the request isn't short-circuited.
    _qhead = _qlen = 0;
    _current = _active = _target = -1;
    _running = false;
    lv_animimg_set_completed_cb(objects.helga, _completedCb);
    _request((HelgaAnim)_desired);

    _status_state = ST_INVALID;   // force a fresh status line (and re-roll) on load
    _updateStatusText();
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

// Record the requested animation, and drive it live only while the overview is
// showing. Bus events call this whether or not the screen is open, so _desired
// always reflects the latest event; _enterOverview replays it on the next load.
// Off-screen we touch nothing but _desired — no animimg work, no rendering.
static void _apply(HelgaAnim anim) {
    _desired = anim;
    if (objects.helga && lv_screen_active() == objects.overview) _request(anim);
    // Party start/stop drives the sprite via play() directly (bypassing onEvent),
    // so refresh the status line here too — _computeStatus reads g_party.active().
    _updateStatusText();
}

void OverviewScreen::play(HelgaAnim anim) {
    _apply(anim);
}

// Hold/release the animation against bus-driven changes. While held, onEvent
// ignores scan/alert events so the animation last set via play() sustains
// (party mode holds HELGA_PARTY for its whole run). play() itself still works
// while held — the caller drives the animation directly. Releasing does not
// restore any state; the caller should play() the desired resume animation.
void OverviewScreen::hold(bool on) {
    _held = on;
}

void OverviewScreen::begin(EventBus& bus) {
    // Helga sniffs while a scan window is open, reacts with the alert animation
    // when a rule fires, and settles to idle when the window closes. These are
    // the signals that actually fire today (PowerManager posts CMD_SCAN_START/
    // STOP per window); switch to EV_SCAN_STATE_CHANGED once ScanEngine emits it.
    bus.subscribe(CMD_SCAN_START,  this);
    bus.subscribe(CMD_SCAN_STOP,   this);
    bus.subscribe(EV_ALERT_RAISED, this);
    // Finer-grained signals for the status line only (the sprite stays on the
    // coarse CMD_SCAN_START/STOP above): per-radio scan state for BLE vs WiFi,
    // and battery level for the low-battery line.
    bus.subscribe(EV_SCAN_STATE_CHANGED, this);
    bus.subscribe(EV_BATTERY_UPDATED,    this);

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
    // Status-line tracking runs regardless of _held: party overrides in
    // _computeStatus, and scan/battery events keep flowing during a party.
    // Any non-alert event is a "state change" that releases a latched alert.
    switch (e.id) {
        case EV_SCAN_STATE_CHANGED:
            if (e.data.scan_state.domain == SCAN_WIFI)
                _wifi_scanning = (e.data.scan_state.active != 0);
            else
                _ble_scanning  = (e.data.scan_state.active != 0);
            _alert_latched = false;
            break;
        case EV_BATTERY_UPDATED:
            _batt_pct      = e.data.battery.pct;
            _alert_latched = false;
            break;
        case EV_ALERT_RAISED:
            _alert_latched = true;
            break;
        case CMD_SCAN_STOP:
            _ble_scanning = _wifi_scanning = false;
            _alert_latched = false;
            break;
        case CMD_SCAN_START:
            _alert_latched = false;
            break;
        default: break;
    }
    _updateStatusText();

    if (_held) return;   // a caller (party mode) owns the animation right now
    switch (e.id) {
        case CMD_SCAN_START:  play(HELGA_SNIFF); break;
        case CMD_SCAN_STOP:   play(HELGA_IDLE);  break;
        case EV_ALERT_RAISED: play(HELGA_ALERT); break;
        default: break;
    }
}
