#include "toast_service.h"
#include "UI/screens.h"
#include "UI/styles.h"     // add_style_alert_card — the toast panel's look
#include <lvgl.h>

ToastService g_toast;

// ---------------------------------------------------------------------------
// One live toast at a time. State is file-static (the anim/timer callbacks are
// plain C function pointers and there's exactly one instance) matching how the
// screen modules in this project hold their LVGL state.
//
// Lifecycle: show() builds the panel → dwell timer → fade anim → lv_obj_del.
// The DELETE event callback is the single place _toast is cleared, so every
// deletion path (fade completion, replacement, an external lv_obj_clean) leaves
// consistent state and can't dangle.
// ---------------------------------------------------------------------------

// Horizontally centered, sitting this many pixels above the bottom of the screen
// (bottom edge to screen edge) — the Android convention. Keeps it out of the dead
// center, where it would cover whatever the operator was just looking at.
static constexpr int32_t TOAST_BOTTOM_GAP_PX = 60;

static lv_obj_t*   _toast       = nullptr;
static lv_timer_t* _toast_timer = nullptr;

// lv_anim_exec_xcb_t is void(*)(void*, int32_t) but lv_obj_set_style_opa takes a
// third `selector` argument, so it can't be cast to the anim signature — the
// selector would be whatever happened to be in the register and the opacity
// would land on an arbitrary part/state. Pass it explicitly.
static void _setOpa(void* obj, int32_t opa) {
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)opa, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void _onDelete(lv_event_t* /*e*/) { _toast = nullptr; }

static void _fadeDone(lv_anim_t* a) {
    lv_obj_del((lv_obj_t*)a->var);   // LV_EVENT_DELETE clears _toast
}

static void _startFade() {
    if (!_toast) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _toast);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, ToastService::FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, _setOpa);
    lv_anim_set_completed_cb(&a, _fadeDone);
    lv_anim_start(&a);
}

static void _dwellDone(lv_timer_t* t) {
    lv_timer_delete(t);
    _toast_timer = nullptr;
    _startFade();
}

// Drop whatever is on screen right now, cancelling its dwell and its fade.
// lv_obj_del deletes the anims targeting the object, so this is safe mid-fade.
static void _clear() {
    if (_toast_timer) { lv_timer_delete(_toast_timer); _toast_timer = nullptr; }
    if (_toast)       { lv_obj_del(_toast);            _toast       = nullptr; }
}

void ToastService::show(const char* text, uint32_t dwell_ms) {
    if (!text || !text[0]) return;
    _clear();

    lv_obj_t* toast = lv_obj_create(lv_layer_top());
    add_style_alert_card(toast);   // theme-tracked panel, same as the alert cards
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -TOAST_BOTTOM_GAP_PX);
    // Never interactive, never focusable: it must not eat a press, and it must
    // not land in groups.UINavigation, which screens rebuild underneath it.
    lv_obj_remove_flag(toast, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                              LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                              LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_event_cb(toast, _onDelete, LV_EVENT_DELETE, nullptr);

    lv_obj_t* label = lv_label_create(toast);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_align(label, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, text);   // copies; caller's buffer needn't outlive this

    _toast       = toast;
    _toast_timer = lv_timer_create(_dwellDone, dwell_ms, nullptr);
}

void ToastService::dismiss() {
    if (!_toast) return;
    if (_toast_timer) { lv_timer_delete(_toast_timer); _toast_timer = nullptr; }
    _startFade();
}

bool ToastService::active() const { return _toast != nullptr; }
