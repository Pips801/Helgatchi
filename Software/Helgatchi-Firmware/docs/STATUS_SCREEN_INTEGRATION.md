# Status screen (mascot) integration

How to drive a Tamagotchi-style mascot on the status screen from firmware
state. Two halves:

1. **Signals** — which bus events / getters tell the mascot what to do.
2. **Rendering** — how to get an Aseprite sprite sheet onto an LVGL/EEZ screen
   without EEZ's per-frame asset bloat.

Read `ARCHITECTURE.md` first for the service model and event bus.

---

## 0. The one constraint that shapes everything

The device is **not always on**. `PowerManager` runs a wake → scan → drain →
sleep loop (`src/power_manager.cpp` `tick()`), and between scheduled scan
windows it **deep-sleeps** — CPU halted, screen off, no code running. The
status screen only renders while the display is on, which happens during an
*interactive* session (button wake) or a wake-screen alert. A timer-driven
scan window runs with the screen off and nobody watching.

So the mascot lives inside interactive sessions, where it sees this repeating
cycle:

```
CMD_SCAN_START ─►[~5s scan]─► CMD_SCAN_STOP ─►[drain]─► EV_SCAN_COMPLETE ─►[~30s idle]─► CMD_SCAN_START …
                                                                                │
                          (after interactive timeout with no button/alert)      ▼
                          EV_SLEEP_COUNTDOWN_UPDATED (counts down) ─► EV_POWER_STATE_CHANGED = POWER_SLEEPING
                                                                       (deep sleep — screen already off)
```

Durations come from settings: `SKEY_SCAN_DURATION_S` (default 5),
`SKEY_SLEEP_DURATION_S` (30), `SKEY_INTERACTIVE_TIMEOUT_S` (30).

---

## 1. Subscribing to the bus

The mascot logic should be a service following the house pattern
(`begin(EventBus&)` / `tick()` / `onEvent()`), subscribing in `begin()`. You
can subscribe to `CMD_*` ids too — the bus dispatches commands to any
subscriber, not just the handler that acts on them.

```cpp
void StatusSpriteService::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(CMD_SCAN_START,            this);
    bus.subscribe(CMD_SCAN_STOP,             this);
    bus.subscribe(EV_SCAN_COMPLETE,          this);
    bus.subscribe(EV_ALERT_RAISED,           this);
    bus.subscribe(EV_ALERT_UPDATED,          this);
    bus.subscribe(EV_ALERT_CLEARED,          this);
    bus.subscribe(EV_SLEEP_COUNTDOWN_UPDATED,this);
    bus.subscribe(EV_POWER_STATE_CHANGED,    this);
    bus.subscribe(EV_BATTERY_UPDATED,        this);
    bus.subscribe(EV_RULE_TRIGGERED_LOCAL,   this);   // party — see §4
    bus.subscribe(EV_UI_ACTIVITY,            this);
    bus.subscribe(EV_TICK_1S,                this);
}
```

Event ids: `include/event_ids.h`. Payload union: `include/event_payload.h`
(fixed 8 bytes, accessed as `e.data.<member>`).

---

## 2. Mascot state → signal map

| Mascot behavior | Enter on | Leave on | Detail / how much |
|---|---|---|---|
| **Sniffing / magnifying glass** (scanning) | `CMD_SCAN_START` | `CMD_SCAN_STOP` | `SKEY_SCAN_MODE` bit0=BLE bit1=WiFi — flavor the search |
| **Paperwork** (crunching the catch) | `CMD_SCAN_STOP` | `EV_SCAN_COMPLETE` | backlog = `g_scan_service.writePos() - g_rules.ringReadPos()` → size of the paper stack |
| **Eating** (alert fired) | `EV_ALERT_RAISED` | `EV_ALERT_CLEARED` or your own timeout | `g_alerts.find(e.data.alert.alert_id)` → title / type / rssi. `EV_ALERT_UPDATED` = another bite (deduped re-fire of same device) |
| **Dancing** (party mode) | `EV_RULE_TRIGGERED_LOCAL` where fired rule `action == RULE_ACTION_PARTY` | your own timeout | ⚠ needs a one-line firmware change today — see §4 |
| **Getting into bed** (about to sleep) | `EV_SLEEP_COUNTDOWN_UPDATED` with small `.seconds` | `POWER_SLEEPING` = lights out | `.seconds` counts down; `0xFFFF` = "won't sleep" (USB/serial attached → stay perky) |
| **Idle / breathing** (between windows) | `EV_SCAN_COMPLETE` | next `CMD_SCAN_START` | `g_power.secondsUntilNextScan()` |
| **Notices you** (interaction) | `EV_UI_ACTIVITY`, `EV_BTN_*` | — | buttons: `EV_BTN_LEFT/RIGHT/CENTER_SHORT/CENTER_LONG/CENTER_HOLD` |
| animation clock | `EV_TICK_1S` (1 Hz) | — | or run your own faster `lv_timer` (see §6) |

**Bedtime timing gotcha:** don't kick off the "climb into bed" animation on
`POWER_SLEEPING`. By the time it's posted the backlight is already off and the
CPU deep-sleeps a few lines later (`_enterSleep()`, `src/power_manager.cpp`).
It *is* dispatched (there's a `_bus->dispatch()` flush before sleep) so it's a
clean "we're going down" marker — but it's too late to render. Play the
climbing-into-bed animation over the final few `EV_SLEEP_COUNTDOWN_UPDATED`
ticks; the screen also dims at ≤5 s remaining, so that's a natural cue window.

---

## 3. Ambient stats (mood, not one-shot events)

| Data | Event | Poll getter |
|---|---|---|
| Battery % / mV | `EV_BATTERY_UPDATED` → `e.data.battery.pct` / `.mv` | `g_power.lastBatteryPct()` / `lastBatteryMv()` |
| Charging state | (same) — `pct` sentinels | `200`=charging, `201`=charged, `202`=missing (`include/power_manager.h`). Low % → tired/hungry; charging → feed animation |
| World crowdedness | — | `g_scan_service.seenCount()` (unique devices seen this session) |
| Lifetime alerts / matches | — | `g_alerts.count()`, `g_rules.totalMatches()` |

---

## 4. Party mode — IMPLEMENTED (`PartyService`)

Party mode is live. It's owned by `PartyService` (`src/party_service.cpp` /
`include/party_service.h`, global `g_party`), a sustained device state rather
than the one-shot the alert path gives you. While active:

- looping rainbow LEDs (`LED_PATTERN_RAINBOW_FAST`);
- rhythmic haptic pulses (re-fires `HAPTIC_DOUBLE_TAP` on an interval, since the
  vibe step machine has no loop primitive);
- Helga's `HELGA_PARTY` dance on the overview screen (held against the scan
  cycle via `OverviewScreen::hold()` so it doesn't get bumped to sniff/idle);
- a colour-cycling "Party!" banner on the top bar, and the left/right status
  icons tinted the **same** cycling hue (`DisplayService::setIconTint()`);
- **sleep inhibited** — `PowerManager::_isInhibited()` returns true while party
  is active, so the device won't deep-sleep until party ends.

Runs for a fixed duration (default 20 s; re-triggering refreshes the timer).

Navigation model:
- Triggering party always brings up the status (overview) page
  (`eez_flow_set_screen`), from wherever you were.
- **Long-press exits party but stays on the status page** — the exit is handled
  in `UIController`'s `EV_BTN_CENTER_LONG` (it checks `g_party.active()` and
  calls `stop()`, then `break`s without navigating). Handling it there rather
  than by party subscribing to the button avoids a race on the active flag
  between two handlers in one dispatch.
- A **second** long-press (party now off, on the overview) backs out to the
  **main menu** — `UIController` special-cases the overview to
  `set_screen(SCREEN_ID_MAIN_MENU)` rather than popping to whatever was open
  when party fired.
- Timeout / `party off` also just tear down and stay on the status page; party
  never navigates on exit.

Two triggers, both funnelling through `g_party.start()/stop()`:

- **Serial**: `party on [secs]` / `party off` (`SerialConsole::_cmdParty`).
- **Rule**: `RulesService::_fire()` calls `g_party.start(..., from_rule=true)`
  for `RULE_ACTION_PARTY` (replacing the old early-return no-op) — no alert card,
  no `EV_ALERT_RAISED`. `data/rules/factory/party.json` is the shipped example.

**Cooldown**: a party beacon is meant to persist for the whole event, so the
rule re-fires every scan. While active that just refreshes the timer. After a
**manual dismiss** (long-press-back, or `party off`) rule triggers are ignored
for `COOLDOWN_MS` (5 min) so a present beacon can't instantly re-trigger — the
user gets a real break. A natural timeout arms no cooldown (the beacon is gone
by definition). An explicit serial `party on` overrides and clears any cooldown.

Party owns its own effects, so a party rule's `vibe`/`led` JSON fields are
ignored (party always uses rainbow + double-tap). It does **not** raise an
alert, so the `EV_RULE_TRIGGERED_LOCAL` / "mascot decides by rule action" route
sketched in earlier drafts was not needed for party — `_fire` calls the service
directly, mirroring how it calls `g_alerts.raise()` for alert rules.

Banner note: the top-bar centre title's *text* is reasserted every flow tick
from the Top Bar widget's `Title` expression (`evalTextProperty` in
`tick_user_widget_top_bar`), so it can't be repurposed from C. `PartyService`
instead hangs its own label on the top-bar container, cycles its colour with
`lv_obj_set_style_text_color` (colour survives the tick), and hides the bound
title while active — zero `.eez-project` / generated-file changes.

---

## 5. Recommended service shape

`StatusSpriteService : IEventHandler` in `src/status_sprite_service.cpp` +
`include/status_sprite_service.h`, `extern StatusSpriteService g_sprite;`.
Init in `main.cpp` after `g_ui`/screens exist (it references generated LVGL
objects). It owns a small state enum + priority rules and its only output is
selecting which animation the mascot image widget plays. Suggested priority
when several signals overlap: `DANCING > EATING > BEDTIME > PAPERWORK >
SNIFFING > NOTICES_YOU > IDLE`.

Keep the screen, layout, top bar, and the mascot *widget itself* owned by EEZ
(house preference is EEZ-first). The service only drives the one image
widget's frame — see §6 for why that specific part must be C.

---

## 6. Rendering an Aseprite sprite sheet in LVGL/EEZ

### Why not EEZ's animated-image widget

EEZ's animation widget cycles a **fixed array of separate image assets** in
order — it can't be told "play frames 8–15 now, 0–7 later," and every frame is
its own `lv_image_dsc_t` baked into the generated `src/UI/images.c`. That's
both the wrong control model for a state machine and a lot of flash (see
sizing below). So the mascot is the one place we bypass EEZ rendering.

### The technique: one image, pan with offset

EEZ has no crop-to-region control, but the underlying **LVGL does** — not by
slicing the sheet, but by *panning* a full sheet behind a frame-sized window.
LVGL draws an image clipped to its widget's bounds, and `lv_image_set_offset_x/y`
shifts the image content inside that window. So a single sprite-sheet image +
a widget sized to one frame + an offset = exactly one frame visible. No
slicing, no per-frame assets, and you address any frame directly (unlike EEZ's
animated-image widget, which can only cycle a fixed list in order).

Think of the widget as a fixed 64×64 hole and the sheet as a big sheet of
paper behind it. The offset slides the paper; whatever lands in the hole is
what's drawn. Offsets are **negative** because moving to a *later* frame means
sliding the sheet left/up (content moves toward negative x/y).

1. In EEZ Studio, drop an **Image** widget on the status screen, name it
   `mascot`, and size it to one frame (e.g. 64×64). Leave its source empty or
   a placeholder — we override at runtime. Named widgets are exposed to C as
   `objects.mascot` (generated in `src/UI/screens.h`).
2. Ship the sprite sheet as **one** `lv_image_dsc_t` (see pipeline below),
   e.g. `extern const lv_image_dsc_t mascot_sheet;` (256×256 = 4×4 grid of
   64×64 frames).
3. Point the widget at the sheet once, then select frames by offset:

```cpp
static constexpr int FW = 64, FH = 64, COLS = 4;   // frame w/h, sheet columns

void StatusSpriteService::_showFrame(int frame) {
    lv_obj_t* m = objects.mascot;
    lv_image_set_src(m, &mascot_sheet);              // once is enough; cheap to repeat
    lv_obj_set_size(m, FW, FH);                       // widget = one frame (enables clipping)
    lv_image_set_inner_align(m, LV_IMAGE_ALIGN_DEFAULT);
    lv_image_set_offset_x(m, -(frame % COLS) * FW);
    lv_image_set_offset_y(m, -(frame / COLS) * FH);
}
```

4. Animate with an `lv_timer` (or step from `EV_TICK_1S` if 1 fps is enough).
   Each animation is just a `{first_frame, count, fps, loop}` descriptor the
   state machine swaps — you decide exactly which frames belong to which state,
   which is the thing EEZ's widget can't do.

**Worked example** — a 256×256 sheet, 4×4 grid of 64×64 frames (`FW=FH=64`,
`COLS=4`). `frame` is the linear index in row-major order:

```
sheet layout            frame → offset (x, y)
┌────┬────┬────┬────┐    frame 0  → (   0,    0)     frame 5  → ( -64,  -64)
│  0 │  1 │  2 │  3 │    frame 1  → ( -64,    0)     frame 6  → (-128,  -64)
├────┼────┼────┼────┤    frame 2  → (-128,    0)     frame 9  → ( -64, -128)
│  4 │  5 │  6 │  7 │    frame 3  → (-192,    0)     frame 15 → (-192, -192)
├────┼────┼────┼────┤
│  8 │  9 │ 10 │ 11 │    x = -(frame % COLS) * FW    (column → horizontal pan)
├────┼────┼────┼────┤    y = -(frame / COLS) * FH    (row    → vertical pan)
│ 12 │ 13 │ 14 │ 15 │
└────┴────┴────┴────┘
```

So frame 6 is column 2, row 1 → offset `(-128, -64)`: the sheet slides 128 px
left and 64 px up, putting the frame-6 cell in the widget's window.

Gotchas: the widget must be **≤** the sheet and no larger than one frame, or
LVGL tiles/repeats the image to fill it; keep `inner_align` at default
(top-left) so offset math starts from `(0, 0)`; export the sheet with no
padding/trim so every frame sits on a constant `FW`/`FH` pitch (Aseprite's
trim option breaks the arithmetic).

### Asset pipeline: keep it tiny and out of EEZ

Aseprite is already palette-based — stay in **Indexed** color mode with ≤16
colors and reserve index 0 as transparent. Then:

1. Aseprite → *File ▸ Export Sprite Sheet* → single PNG, fixed grid, no
   padding/trim (constant frame pitch is what the offset math relies on).
2. Convert to an LVGL C array in an **indexed** format — `LV_COLOR_FORMAT_I4`
   (16-color, 4 bpp) via LVGL 9's `LVGLImage.py`:
   `python LVGLImage.py --cf I4 --ofmt C mascot_sheet.png`.
   Indexed formats carry an ARGB8888 palette, so per-index alpha (transparent
   background) works; I2 (4-color) is even smaller if the art allows.
3. Keep the generated `.c` **out** of the EEZ `src/UI/images.c` table — it's
   its own source file, referenced only by the sprite service. Better: make it
   a build-time step (`scripts/build_sprites.py`, mirroring
   `build_vendor_tables.py`) so generated art isn't hand-committed and rebuilds
   from the Aseprite PNG. It stays `const` → memory-mapped from flash, drawn
   directly, **zero runtime decode** (skip RLE/LZ4 compression — an I4 sheet is
   already small and compression would force a full-sheet decode into RAM).

### Why this is dramatically smaller

Per 64×64 frame:

| Format | Bytes/frame | 48 frames (6 states × 8) |
|---|---|---|
| ARGB8888 (worst case) | 16 KB | 768 KB |
| RGB565 (typical EEZ import) | 8 KB | 384 KB |
| **I4** (16-color, +64 B palette) | **2 KB** | **~96 KB** |
| I2 (4-color, +16 B palette) | 1 KB | ~48 KB |

I4 as a single sheet is ~4× smaller than what EEZ would generate as separate
RGB565 assets, plus it's one asset the EEZ pipeline never touches.

---

## 7. Open decisions for the team

- **Frame/grid size** (drives §6 constants and the EEZ widget size).
- **Does party buzz + flash?** (keep the alert-raise for party — §4).
- **Alert "eating" duration** — clear on `EV_ALERT_CLEARED` (user ack) or a
  fixed on-screen timeout? Alerts persist until ack'd today.
- **Idle fidgets** — random secondary animations off `EV_TICK_1S` when in IDLE.

## Quick links

- Signals: `include/event_ids.h`, `include/event_payload.h`, `include/event_bus.h`
- Power/lifecycle: `src/power_manager.cpp`, `include/power_manager.h`
- Alerts: `include/alerts_service.h`; Rules: `include/rules_service.h`
- Scan (device counts): `include/scan_service.h`
- EEZ generated objects: `src/UI/screens.h` (`objects.<name>`), images: `src/UI/images.c`
