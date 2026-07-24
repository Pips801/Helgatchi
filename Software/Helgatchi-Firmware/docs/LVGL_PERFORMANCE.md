# LVGL / LovyanGFX Performance on ESP32-S3

How the Helgatchi UI went from 1-3 FPS to 20+ FPS on the heaviest screen, and
40-50 FPS on menus — on the same hardware, with the same visual design.

Every technique here is transferable. The stack: ESP32-S3 (240 MHz, 8 MB
QSPI/OPI PSRAM), ST7789 280×240 over 80 MHz SPI, LVGL 9.5 software renderer,
LovyanGFX panel driver, Arduino framework under PlatformIO. The defaults of
this stack are **capacity-minded, not performance-minded**: they favor "fits
in any RAM budget" (PSRAM everything) and "smallest binary" (`-Os`) over frame
rate. Almost everything below is undoing one of those defaults deliberately.

Measured journey on the devices screen (a scrollable list of 140+ cards):

| Change round                                          | Scroll FPS |
| ----------------------------------------------------- | ---------- |
| Baseline (all defaults)                                | 1-3        |
| Memory placement + `-O2` + style cache                 | ~5         |
| Recycler (constant widget count)                       | ~15        |
| Buffer-size fix + zero-copy swapped DMA                | 15-20      |
| Radius 0 + label merge + scroll-anim cap               | 20+        |

Menus (light screens) went from 20-30 to 40-50 FPS from the global changes
alone — they had the same memory-placement tax, just fewer objects paying it.

---

## 0. Measure before touching anything

Nothing below was guessed; each round targeted the number that was actually
biggest. The instrumentation that made that possible:

- **LVGL's perf monitor** (`LV_USE_SYSMON` + `LV_USE_PERF_MONITOR`) for FPS
  and CPU%, gated behind a debug setting so it ships disabled.
- **Render vs flush split**: wall-time inside the flush callback is
  accumulated separately (`_flush_us_total` in `ui_controller.cpp`), and
  `tick()` diffs it per frame. "Render 240 ms, flush 11 ms" pointed at
  rasterization; the reverse would have pointed at the SPI link.
- **Loop-phase telemetry**: microseconds per service per loop iteration
  (scan, rules, UI, …) over teleplot. This ruled out the alternative
  hypothesis that the UI was being *starved* by the scan engine rather than
  being slow itself.

Two diagnostic questions did most of the aiming:

1. **Does FPS degrade with total content, or only visible content?** Visible
   content is constant on a list screen (~6 cards fit), so "more devices =
   slower" meant the cost was structural (per-object overhead), not pixel
   work. That conclusion is what justified the recycler.
2. **Does FPS recover when idle?** Idle-recovers means the cost is the redraw
   itself; idle-also-bad means something is invalidating continuously (timers,
   data refreshes) and you have a different bug.

---

## 1. Memory placement is the whole game on ESP32-S3

The single biggest lesson. The S3's PSRAM sits behind a QSPI/OPI bus at
80 MHz with a small data cache in front. Anything the renderer touches per
pixel or per object **must not live there**.

### Draw buffers → internal SRAM, DMA-capable

The software renderer *read-modify-writes* the strip buffer for every blend:
anti-aliased glyph edges, rounded corners, borders, any opacity. A render
strip larger than the data cache degrades every one of those accesses to
PSRAM bus speed. This alone was the difference between ~1 FPS and ~15 FPS.

```cpp
buf = heap_caps_malloc(BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
```

Partial-render strips keep the footprint sane: 3 strips of 80 rows
(2 × 44.8 KB) instead of a full framebuffer. Fewer, larger strips = fewer
tree walks per frame and fewer tear seams; the limit is internal heap.

### The `lv_color_t` sizing trap (LVGL 9)

In LVGL 9, `lv_color_t` is **always a 3-byte RGB888 struct**, regardless of
`LV_COLOR_DEPTH`. Sizing RGB565 buffers with `sizeof(lv_color_t)` silently
over-allocates them by 50% — LVGL just quietly makes taller strips out of the
extra bytes, so nothing visibly breaks and the RAM is gone. Use
`lv_color16_t` (or an explicit `* 2`).

### The LVGL heap can stay in PSRAM — with a mitigation

512 KB of widget pool doesn't fit in internal SRAM, and unlike the draw
buffer, object memory is touched hundreds of times per frame rather than
millions. It stays in PSRAM (`LV_MEM_POOL_ALLOC` → `heap_caps_malloc(...,
MALLOC_CAP_SPIRAM)`), and two things blunt the cost:

- `LV_OBJ_STYLE_CACHE 1` — 8 bytes per object to skip most style-list
  walks, which are pure pointer-chasing through PSRAM during draw.
- Keeping the object *count* small (§3), so the walk that does happen is
  short.

### Know that IRAM and heap are the same pool

`LV_ATTRIBUTE_FAST_MEM = IRAM_ATTR` looks tempting (hot render loops out of
flash cache), but on the S3 instruction RAM and data RAM come out of the same
512 KB SRAM. It converts scarce heap into a modest icache win. Rejected here;
only worth it if you have RAM to burn.

---

## 2. The flush path: overlap everything, copy nothing

### Double-buffer + deferred `endWrite`

Two equal strips let LVGL rasterize strip N+1 while strip N is on the DMA
bus. The flush callback intentionally does **not** wait for the transfer: it
starts the DMA, calls `lv_display_flush_ready()` immediately, and the *next*
flush drains the previous transfer before reusing the bus. The SPI link runs
essentially 100% duty during a frame.

### If you DMA from PSRAM, write back the cache

The CPU writes pixels through the data cache; GDMA reads physical PSRAM.
Without `Cache_WriteBack_Addr()` (rounded out to 32-byte lines) before each
transfer, the DMA reads stale bytes — the classic green-glitch corruption.
Internal SRAM is DMA-coherent; this cost disappears when buffers move there
(keep it on the fallback path only).

### Render in the panel's byte order — zero-copy DMA

RGB565 panels want MSB-first; LVGL renders CPU-native little-endian by
default. LovyanGFX's `writePixelsDMA(..., swap=true)` fixes that by routing
every strip through a **CPU swap-copy into its own DMA buffer**. The better
fix is to make the swap never happen:

```cpp
lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);  // LVGL side
tft.writePixelsDMA((uint16_t*)px_map, px_count, /*swap=*/false);    // flush side
```

LVGL 9's SW renderer supports `RGB565_SWAPPED` natively (including image and
font blending — sprites, indexed images, and QR canvases all verified fine).
These two lines must flip together or every color mashes.

### Know your wire ceiling

280×240×16 bit at 80 MHz SPI ≈ 13.4 ms per full-screen push ≈ 74 FPS
absolute ceiling for full redraws. Worth computing early — it tells you when
flush will become the next bottleneck and whether panel-link upgrades matter.

### FULL render mode: tried, reverted

One flush per frame reduced tear seams from 2 to 1 but the seam was still
visible during scrolls, and small isolated updates paid a full-screen cost.
PARTIAL with pipelined strips won.

---

## 3. LVGL cost scales with objects that *exist*, not objects you *see*

The most counter-intuitive lesson, and the biggest single win.

Off-screen widgets are clipped before drawing, but they are **not free**:
every refresh walks the tree per strip (coordinate reads, clip tests, cover
checks), every scroll step updates coordinates on every child recursively,
and every invalidation clips against the whole child list. With objects in
PSRAM, each touch is a cache miss. A 140-card list (≈900 objects) cost
~240 ms/frame while drawing only 6 cards.

### The recycler (fixed pool + spacers)

Replace one-widget-per-row with:

```
[ top spacer ][ card 0..11 (pool) ][ bottom spacer ]
```

- **Data lives in a plain C array** (the "rows"): rebuild + re-sort on data
  events costs microseconds and zero LVGL work.
- **The pool binds to a sliding window** around the selection; binding is
  just label-text writes, off the per-frame path entirely.
- **Spacers stand in for off-window rows**, sized `rows_outside × pitch −
  pad_row` (the correction returns the flex gap the spacer itself inserts).
  This makes every row's Y and the total content height **pixel-identical to
  a real full list** — scroll range, scrollbar, and snap behave as if all
  rows existed. This is what a naive push/pop virtualization gets wrong: the
  content height keeps changing and the scroll position "snaps around".
- Derive the pitch from live styles (widget height + container `pad_row`) at
  pool-build time so a designer-side tweak can't desync the math.
- **Selection becomes a data index**, not an LVGL focus object — widgets are
  reused, so "the focused widget" no longer identifies a row. Pin the
  selection by stable identity (here: domain+MAC) so re-sorts follow the
  item. The bound card just gets `LV_STATE_FOCUS_KEY` so existing styles
  render unchanged.

Bonus: deleting per-row widget creation also deletes the entire class of
"widget pool exhausted mid-build" failure modes and their workaround code.

---

## 4. Do less work: guard, defer, throttle

A batch of small patterns that each remove invisible-but-real work:

- **`set_text` is never free.** Even with identical text it reallocs,
  re-measures, and invalidates. Guard every programmatic label write:
  ```cpp
  if (strcmp(lv_label_get_text(lbl), s) != 0) lv_label_set_text(lbl, s);
  ```
  Same for `lv_label_set_long_mode` — no same-value early-out; an unguarded
  call kills and restarts the scroll animation every time.
- **Defer data refreshes while a scroll animation runs**
  (`lv_anim_get(container, NULL)` + a short retry timer). A rebind
  mid-glide fights the animation for frame time and yanks the viewport.
- **Throttle live-updating labels, and only touch what's on screen.** The
  "Ns ago" tick went from every-second-every-card to every-5s-bound-cards.
  Data freshness policy belongs to the data event (RSSI updates only when a
  scan lands), not to a UI timer.
- **Expensive per-item animations only on the selected item.**
  `SCROLL_CIRCULAR` marquee is a continuous invalidation; run it on the one
  selected card, `DOTS` everywhere else. LVGL only starts the marquee when
  text actually overflows, so it's free when not needed.
- `lv_obj_move_to_index` no-ops when the index is unchanged, but real moves
  dirty the parent's flex layout — avoid reorder churn on jittery sort keys.

---

## 5. Cheap render wins that don't change the design

- **`-O2` instead of Arduino's default `-Os`** (`build_unflags = -Os`). The
  blend/blit inner loops are exactly the code size-optimization hurts. Bonus:
  changing build flags forces a full rebuild, which also flushes PlatformIO's
  stale-library-archive footgun for `lv_conf.h` changes.
- **Corner radius 0 on opaque list items.** Not about corner-mask cost: a
  fully opaque *rectangular* widget passes LVGL's cover check, so the parent
  background underneath is never painted. Rounded corners force the parent to
  be drawn and then covered — full overdraw per item, every frame.
- **Merge labels that share a font.** `LV_SYMBOL_*` icons are ordinary glyphs
  in the Montserrat fonts — an icon label + name label at the same size is
  strictly worse than one label with the symbol inline. Each merged label is
  one fewer object in every tree walk and one fewer draw task. (Inline font
  *size* changes are impossible in a label — font is a per-part style;
  recolor `#RRGGBB #` is color-only. Mixed sizes need `lv_spangroup`.)
- **Cap scroll-glide duration.** LVGL scrolls at 120 px/s clamped to
  `[SCROLL_ANIM_TIME_MIN, SCROLL_ANIM_TIME_MAX]` ms (200/400 defaults,
  `#ifndef`-guarded → override via `-D` build flags). Every animated frame is
  a full redraw of the moved area, so a 400 ms glide at 20 FPS is ~8 redraws
  per keypress. 300 ms tested as the feel/cost compromise here. These defines
  are global; a per-page custom `lv_anim` glide was tried and reverted — it
  saved nothing (same per-frame scroll work) and felt worse.
- Disable per-label features you don't use (`LV_LABEL_TEXT_SELECTION 0` on a
  touchless device).

---

## 6. Tried and rejected (so nobody re-treads)

| Idea | Verdict |
| ---- | ------- |
| FULL render mode for tear-free scrolls | 1 seam remained, small updates got slower — reverted |
| Per-page scroll animation via custom `lv_anim` | Same per-frame cost as stock; global clamp is simpler — reverted |
| `LV_ATTRIBUTE_FAST_MEM = IRAM_ATTR` | IRAM and heap share the S3's SRAM; trades scarce heap for a small icache win |
| Hidden-flag / push-pop list virtualization | Flex height changes as items appear → scroll position snaps; spacer-recycler solves it |
| Moving the 512 KB LVGL pool to internal SRAM | Doesn't fit; style cache + low object count is the practical substitute |
| Runtime CPU frequency scaling (earlier session) | `setCpuFrequencyMhz` hangs this board (MSPI/flash+PSRAM timing) — dead idea, see `docs/CPU_SCALING.md` |
| Capping the device list to top-N | Product call: the list must show everything |

---

## 7. Remaining headroom (if ever needed)

- Fewer/simpler labels per card (2-line composition instead of 4 labels).
- `LV_ANIM_OFF` selection stepping — one redraw per keypress, zero glide
  cost; a feel tradeoff, not a technical one.
- Scrollbar off (deliberately kept: it communicates list length).
- Espressif's Xtensa SIMD (PIE) blend routines via `LV_USE_DRAW_SW_ASM` —
  real gains, painful integration under Arduino/PlatformIO.
- SPI link is good to ~74 FPS full-frame; not the bottleneck yet.
