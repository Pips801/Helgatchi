#include "rules_screen.h"
#include "rules_service.h"
#include "toast_service.h"
#include "settings_service.h"   // SKEY_SCAN_MODE — toastRuleRadioWarning
#include "settings_keys.h"
#include "scan_types.h"         // SCAN_BLE / SCAN_WIFI bit positions
#include "event_ids.h"
#include "UI/screens.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
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
// mutation, so serial-console toggles and tag bulk-toggles flip switches.
// ---------------------------------------------------------------------------

struct RuleCard {
    lv_obj_t* panel = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* sw    = nullptr;
    char      name[sizeof(Rule::name)] = {};
};

struct TagCard {
    lv_obj_t* panel = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* sw    = nullptr;
    char      tag[24] = {};
};

static RuleCard _cards[RulesService::MAX_RULES];
static uint16_t _card_count = 0;

static TagCard  _tag_cards[16];
static uint16_t _tag_card_count = 0;

static bool     _inhibit = false;   // guards switch callbacks during programmatic sync

static void _setSwitch(lv_obj_t* sw, bool on) {
    if (!sw) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

// Shared by the rule and tag variants below. `needs` is a radio mask in
// SKEY_SCAN_MODE's bit layout; `what` completes "Enable <radio> scanning for
// <what>" and so reads as the requirement, not a prediction.
//
// Radios are named "BLE" and "WiFi" to match the Settings switch labels exactly
// ("BLE Scanning" / "WiFi Scanning") — the toast should name the thing the
// operator is about to go looking for.
static bool _radioWarning(uint8_t needs, const char* what) {
    // Same bit layout on both sides (bit 0 = BLE, bit 1 = WiFi), so this is plain
    // mask subtraction: what's keyed on, minus what's switched on.
    const uint8_t enabled = (uint8_t)(g_settings.get(SKEY_SCAN_MODE) & 0x3u);

    // Radio-agnostic criteria only (oui / mac / name / oui_org) — no PARTICULAR
    // radio is required, just one of them, so this is only a problem when nothing
    // at all is running. "or" rather than "and": either switch fixes it.
    if (needs == 0) {
        if (enabled) return false;
        g_toast.show("Enable BLE or WiFi scanning\nin Settings");
        return true;
    }

    const uint8_t missing = (uint8_t)(needs & ~enabled);
    if (!missing) return false;   // every radio that's keyed on is running

    // Name the actual set. Both bits are reachable: a single rule can key on both
    // (Flock matches a WiFi ie_sig AND BLE mfg data) and a tag's union routinely
    // does, so with no radio enabled at all the honest answer is "BLE and WiFi".
    const bool ble  = missing & (1u << SCAN_BLE);
    const bool wifi = missing & (1u << SCAN_WIFI);
    const char* radios = (ble && wifi) ? "BLE and WiFi" : (wifi ? "WiFi" : "BLE");

    // Break after "scanning" so the longest first line ("Enable BLE and WiFi
    // scanning", 28 chars) still fits the content-sized panel.
    char buf[64];
    snprintf(buf, sizeof(buf), "Enable %s scanning\nfor %s", radios, what);
    g_toast.show(buf);
    return true;
}

bool toastRuleRadioWarning(const char* rule_name) {
    if (!rule_name || !rule_name[0]) return false;
    return _radioWarning(g_rules.ruleRadioMask(rule_name), "this rule to work");   // "...for this rule to work"
}

bool toastTagRadioWarning(const char* tag) {
    if (!tag || !tag[0]) return false;
    return _radioWarning(g_rules.tagRadioMask(tag), "all rules in this tag");
}

// Switch VALUE_CHANGED → RulesService. The resulting EV_RULES_CHANGED echo
// re-syncs us on the next dispatch (idempotent under _inhibit).
static void _onSwitchChanged(lv_event_t* e) {
    if (_inhibit) return;
    auto* card = (RuleCard*)lv_event_get_user_data(e);
    const bool checked = lv_obj_has_state(card->sw, LV_STATE_CHECKED);
    if (!g_rules.setEnabled(card->name, checked)) {
        _inhibit = true;
        _setSwitch(card->sw, !checked);
        _inhibit = false;
        return;
    }
    // Turning a rule ON is worthless if the radio it keys on is switched off —
    // say so at the moment of the decision. Only on enable: disabling needs no
    // caveat, and the switch itself is the confirmation either way.
    if (checked) toastRuleRadioWarning(card->name);
}

static void _onTagSwitchChanged(lv_event_t* e) {
    if (_inhibit) return;
    auto* card = (TagCard*)lv_event_get_user_data(e);
    const bool checked = lv_obj_has_state(card->sw, LV_STATE_CHECKED);
    g_rules.setTagEnabled(card->tag, checked);
    // One toast for the whole tag, not one per rule it just enabled — the union of
    // the tag's radio requirements answers the same question without strobing the
    // screen through a dozen cards.
    if (checked) toastTagRadioWarning(card->tag);
}

static void _buildCard(lv_obj_t* parent, RuleCard* out) {
    lv_obj_t** slots = (lv_obj_t**)&objects;
    lv_obj_t*  saved[3];
    for (int i = 0; i < 3; i++) saved[i] = slots[i];

    create_user_widget_rule(parent, nullptr, 0);

    out->panel = slots[0];
    out->label = slots[1];
    out->sw    = slots[2];

    for (int i = 0; i < 3; i++) slots[i] = saved[i];

    lv_obj_set_width(out->label, LV_PCT(72));
    lv_label_set_long_mode(out->label, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_add_event_cb(out->sw, _onSwitchChanged, LV_EVENT_VALUE_CHANGED, out);
}

static void _buildTagCard(lv_obj_t* parent, TagCard* out) {
    lv_obj_t** slots = (lv_obj_t**)&objects;
    lv_obj_t*  saved[3];
    for (int i = 0; i < 3; i++) saved[i] = slots[i];

    create_user_widget_rule(parent, nullptr, 0);

    out->panel = slots[0];
    out->label = slots[1];
    out->sw    = slots[2];

    for (int i = 0; i < 3; i++) slots[i] = saved[i];

    lv_obj_set_width(out->label, LV_PCT(72));
    lv_label_set_long_mode(out->label, LV_LABEL_LONG_MODE_DOTS);

    // Place tag cards right before the individual_rules_header inside rules_container
    if (objects.individual_rules_header) {
        int32_t header_idx = lv_obj_get_index(objects.individual_rules_header);
        if (header_idx >= 0) {
            lv_obj_move_to_index(out->panel, header_idx);
        }
    }

    lv_obj_add_event_cb(out->sw, _onTagSwitchChanged, LV_EVENT_VALUE_CHANGED, out);
}

static void _populateNavGroup() {
    for (uint16_t i = 0; i < _tag_card_count; i++) {
        if (_tag_cards[i].sw) lv_group_add_obj(groups.UINavigation, _tag_cards[i].sw);
    }
    for (uint16_t i = 0; i < _card_count; i++) {
        if (_cards[i].sw) lv_group_add_obj(groups.UINavigation, _cards[i].sw);
    }
}

static void _updateChrome() {
    const uint16_t n_rules = g_rules.count();
    const uint16_t n_tags  = _tag_card_count;

    lv_label_set_text_fmt(objects.individual_rules_header,
                          "%u INDIVIDUAL RULE%s", (unsigned)n_rules, n_rules == 1 ? "" : "S");
    lv_label_set_text_fmt(objects.rule_tag_filters_header,
                          "%u RULE TAG FILTER%s", (unsigned)n_tags, n_tags == 1 ? "" : "S");

    if (n_rules > 0) lv_obj_add_flag(objects.no_rulesets_label, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_remove_flag(objects.no_rulesets_label, LV_OBJ_FLAG_HIDDEN);
    if (n_tags > 0)  lv_obj_add_flag(objects.no_tags_label, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_remove_flag(objects.no_tags_label, LV_OBJ_FLAG_HIDDEN);
}

static bool _cardsMatchRules() {
    if (_card_count != g_rules.count()) return false;
    for (uint16_t i = 0; i < _card_count; i++) {
        const Rule* r = g_rules.get(i);
        if (!r || strcasecmp(_cards[i].name, r->name) != 0) return false;
    }
    return true;
}

static void _rebuild() {
    // Clean old tag cards
    for (uint16_t i = 0; i < _tag_card_count; i++) {
        if (_tag_cards[i].panel && lv_obj_is_valid(_tag_cards[i].panel)) {
            lv_obj_del(_tag_cards[i].panel);
        }
        _tag_cards[i] = TagCard{};
    }
    _tag_card_count = 0;

    // Clean old rule cards
    for (uint16_t i = 0; i < _card_count; i++) {
        if (_cards[i].panel && lv_obj_is_valid(_cards[i].panel)) {
            lv_obj_del(_cards[i].panel);
        }
        _cards[i] = RuleCard{};
    }
    _card_count = 0;

    // Build Tag filter cards
    char tags[16][24];
    uint16_t num_tags = g_rules.getUniqueTags(tags, 16);
    for (uint16_t i = 0; i < num_tags; i++) {
        TagCard* card = &_tag_cards[_tag_card_count];
        _buildTagCard(objects.rules_container, card);
        strncpy(card->tag, tags[i], sizeof(card->tag) - 1);
        lv_label_set_text(card->label, tags[i]);
        _inhibit = true;
        _setSwitch(card->sw, g_rules.isTagEnabled(tags[i]));
        _inhibit = false;
        _tag_card_count++;
    }

    // Build Rule cards
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

static void _refresh() {
    // Re-sync tag switches
    _inhibit = true;
    for (uint16_t i = 0; i < _tag_card_count; i++) {
        _setSwitch(_tag_cards[i].sw, g_rules.isTagEnabled(_tag_cards[i].tag));
    }
    _inhibit = false;

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

    _rebuild();
    _updateChrome();

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
