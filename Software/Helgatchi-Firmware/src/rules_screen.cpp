#include "rules_screen.h"
#include "rules_service.h"
#include "event_ids.h"
#include "UI/screens.h"
#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <strings.h>

RulesScreen g_rules_screen;

// ---------------------------------------------------------------------------
// Rules list — dynamic cards rendered into objects.rules_container.
//
// Architecture: EEZ owns the visuals (Rule user widget design, container
// layout). C owns the data flow — one card per loaded ruleset, labelled with
// the ruleset's `name` (the machine id the serial console uses). The switch
// calls g_rules.setEnabled(); RulesService posts EV_RULES_CHANGED on every
// mutation, so serial-console toggles (and the upcoming tag bulk-toggles)
// flip the switches in place.
//
// Cards append to the end of the container's flex column — after
// no_rulesets_label, i.e. under the "INDIVIDUAL RULES" header.
// ---------------------------------------------------------------------------

struct RuleCard {
    lv_obj_t* panel = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* sw    = nullptr;
    char      name[sizeof(Rule::name)] = {};
};

static RuleCard _cards[RulesService::MAX_RULES];
static uint16_t _card_count = 0;
static bool     _inhibit    = false;   // guards switch callbacks during programmatic sync

static void _setSwitch(lv_obj_t* sw, bool on) {
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

// Switch VALUE_CHANGED → RulesService. The resulting EV_RULES_CHANGED echo
// re-syncs us on the next dispatch (idempotent under _inhibit).
static void _onSwitchChanged(lv_event_t* e) {
    if (_inhibit) return;
    auto* card = (RuleCard*)lv_event_get_user_data(e);
    const bool checked = lv_obj_has_state(card->sw, LV_STATE_CHECKED);
    if (!g_rules.setEnabled(card->name, checked)) {
        // Stale card (ruleset vanished mid-frame) — revert the switch; the
        // pending EV_RULES_CHANGED rebuild will reconcile the list.
        _inhibit = true;
        _setSwitch(card->sw, !checked);
        _inhibit = false;
    }
}

// Instantiate the EEZ-designed Rule user widget so its layout and styles come
// straight from create_user_widget_rule() in src/UI/screens.c — the single
// place to edit them. The generated builder stores its child pointers into
// objects[startWidgetIndex + 0..2]; the widget isn't placed on a screen (so
// it has no reserved slots) and is never ticked by EEZ, so we lend it slots
// [0..2] for the call, read the created objects back, then restore the
// originals. Index mapping matches create_user_widget_rule(): +0 panel,
// +1 label, +2 switch.
static void _buildCard(lv_obj_t* parent, RuleCard* out) {
    lv_obj_t** slots = (lv_obj_t**)&objects;
    lv_obj_t*  saved[3];
    for (int i = 0; i < 3; i++) saved[i] = slots[i];

    create_user_widget_rule(parent, nullptr, 0);

    out->panel = slots[0];
    out->label = slots[1];
    out->sw    = slots[2];

    for (int i = 0; i < 3; i++) slots[i] = saved[i];

    // Cap the label short of the switch so a long ruleset name ellipsizes
    // instead of running underneath it.
    lv_obj_set_width(out->label, LV_PCT(72));
    lv_label_set_long_mode(out->label, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_add_event_cb(out->sw, _onSwitchChanged, LV_EVENT_VALUE_CHANGED, out);
}

// Keypad nav: EEZ's rules screen-load handler clears groups.UINavigation, so
// re-add every card switch in visual order (alerts-screen pattern).
static void _populateNavGroup() {
    for (uint16_t i = 0; i < _card_count; i++) {
        if (_cards[i].sw) lv_group_add_obj(groups.UINavigation, _cards[i].sw);
    }
}

static void _updateChrome() {
    const uint16_t n_rules = g_rules.count();
    const uint16_t n_tags  = 0;   // no tags yet — wired for the tags pass

    lv_label_set_text_fmt(objects.individual_rules_header,
                          "%u INDIVIDUAL RULE%s", (unsigned)n_rules, n_rules == 1 ? "" : "S");
    lv_label_set_text_fmt(objects.rule_tag_filters_header,
                          "%u RULE TAG FILTER%s", (unsigned)n_tags, n_tags == 1 ? "" : "S");

    if (n_rules > 0) lv_obj_add_flag(objects.no_rulesets_label, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_remove_flag(objects.no_rulesets_label, LV_OBJ_FLAG_HIDDEN);
    if (n_tags > 0)  lv_obj_add_flag(objects.no_tags_label, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_remove_flag(objects.no_tags_label, LV_OBJ_FLAG_HIDDEN);
}

// True when the loaded ruleset list still matches the cards one-to-one (same
// count, same names in order) — i.e. only enabled state can have changed.
static bool _cardsMatchRules() {
    if (_card_count != g_rules.count()) return false;
    for (uint16_t i = 0; i < _card_count; i++) {
        const Rule* r = g_rules.get(i);
        if (!r || strcasecmp(_cards[i].name, r->name) != 0) return false;
    }
    return true;
}

static void _rebuild() {
    for (uint16_t i = 0; i < _card_count; i++) {
        if (_cards[i].panel && lv_obj_is_valid(_cards[i].panel)) {
            lv_obj_del(_cards[i].panel);
        }
        _cards[i] = RuleCard{};
    }
    _card_count = 0;

    const uint16_t n = g_rules.count();
    for (uint16_t i = 0; i < n && i < RulesService::MAX_RULES; i++) {
        const Rule* r = g_rules.get(i);
        if (!r) continue;
        RuleCard* card = &_cards[_card_count];
        _buildCard(objects.rules_container, card);
        strncpy(card->name, r->name, sizeof(card->name) - 1);
        lv_label_set_text(card->label, r->name);
        _inhibit = true;
        _setSwitch(card->sw, r->enabled);
        _inhibit = false;
        _card_count++;
    }
}

// Sync UI to g_rules. Same ruleset set → just re-sync switch states (the
// common case: a serial or tag toggle; preserves scroll position). Set
// changed (create/delete/reload) → full rebuild.
static void _refresh() {
    if (_cardsMatchRules()) {
        _inhibit = true;
        for (uint16_t i = 0; i < _card_count; i++) {
            const Rule* r = g_rules.get(i);
            if (r) _setSwitch(_cards[i].sw, r->enabled);
        }
        _inhibit = false;
    } else {
        _rebuild();
        if (lv_screen_active() == objects.rules) {
            lv_group_remove_all_objs(groups.UINavigation);
            _populateNavGroup();
        }
    }
    _updateChrome();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RulesScreen::begin(EventBus& bus) {
    bus.subscribe(EV_RULES_CHANGED, this);

    // g_rules loads before g_ui in setup(), so the list is ready now.
    _rebuild();
    _updateChrome();

    // Repopulate the keypad nav group with the card switches when the rules
    // screen loads (EEZ's own handler clears the group first), and apply any
    // change that arrived while another screen was active.
    if (objects.rules) {
        lv_obj_add_event_cb(objects.rules, [](lv_event_t* /*e*/) {
            if (g_rules_screen._dirty) {
                g_rules_screen._dirty = false;
                _refresh();
            }
            _populateNavGroup();
        }, LV_EVENT_SCREEN_LOAD_START, nullptr);
    }
}

void RulesScreen::onEvent(const Event& e) {
    if (e.id != EV_RULES_CHANGED) return;
    if (lv_screen_active() == objects.rules) _refresh();
    else                                     _dirty = true;
}
