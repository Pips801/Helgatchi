#include "party_service.h"
#include "event_ids.h"
#include "led_service.h"
#include "vibe_service.h"
#include "overview_screen.h"
#include "display_service.h"
#include "hal.h"
#include "UI/screens.h"
#include "UI/eez-flow.h"
#include <Arduino.h>
#include <lvgl.h>

PartyService g_party;

namespace {

void partyPanelClicked(lv_event_t*) {
    g_party.startMenu();
}

}  // namespace

static constexpr HapticPatternId PARTY_VIBE = HAPTIC_DOUBLE_TAP;
static constexpr uint32_t VIBE_INTERVAL_MS = 700;
static constexpr uint32_t TEXT_INTERVAL_MS = 60;
static constexpr uint16_t HUE_STEP = 10;
static constexpr uint32_t AWAKE_INTERVAL_MS = 1000;
static const char* const BANNER_TEXT = "Party!";

#define OVERVIEW_TITLE objects.obj9__top_bar_center_text

static PartyScreen _currentPartyScreen() {
    lv_obj_t* active = lv_screen_active();
    if (active == objects.overview) return PartyScreen::OVERVIEW;
    if (active == objects.helga_menu) return PartyScreen::HELGA_MENU;
    return PartyScreen::OTHER;
}

void PartyService::begin(EventBus& bus) {
    _bus = &bus;

    if (!objects.party_panel) {
        Serial.println("[party] generated Party panel is unavailable");
        return;
    }

    lv_obj_add_event_cb(objects.party_panel,
                        partyPanelClicked,
                        LV_EVENT_CLICKED,
                        nullptr);
}

void PartyService::onEvent(const Event&) {}

uint32_t PartyService::remainingMs() const {
    return _session.remainingMs(millis());
}

void PartyService::_ensureBanner() {
    if (_banner) return;
    lv_obj_t* title = OVERVIEW_TITLE;
    if (!title) return;
    lv_obj_t* parent = lv_obj_get_parent(title);
    if (!parent) return;

    lv_obj_t* b = lv_label_create(parent);
    lv_obj_set_style_align(b, LV_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(b, BANNER_TEXT);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    _banner = b;
}

void PartyService::_refreshColors() {
    _hue = (uint16_t)((_hue + HUE_STEP) % 360);
    lv_color_t c = lv_color_hsv_to_rgb(_hue, 100, 100);
    if (_banner) lv_obj_set_style_text_color((lv_obj_t*)_banner, c, LV_PART_MAIN | LV_STATE_DEFAULT);
    uint32_t rgb = ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | (uint32_t)c.blue;
    g_display.setIconTint(rgb);
}

void PartyService::start(uint32_t duration_ms, bool from_rule) {
    const uint32_t now = millis();

    if (from_rule) {
        if (_cooldown.active(now, COOLDOWN_MS)) return;
    } else {
        _cooldown.clear();
    }

    if (duration_ms == 0) duration_ms = DEFAULT_DURATION_MS;

    const bool was_active = _session.active();
    _session.startTimed(now, duration_ms);
    if (!was_active) _activate(now);
}

void PartyService::startMenu() {
    const uint32_t now = millis();
    _cooldown.clear();

    const bool was_active = _session.active();
    _session.startMenu();
    if (!was_active) _activate(now);
}

void PartyService::_activate(uint32_t now_ms) {
    _navigation.reset();
    _last_vibe_ms = _last_text_ms = _last_awake_ms = now_ms;

    g_hal.wakeDisplay();
    if (lv_screen_active() != objects.overview) {
        eez_flow_set_screen(SCREEN_ID_OVERVIEW,
                            LV_SCR_LOAD_ANIM_FADE_IN,
                            200,
                            0);
    }

    g_overview_screen.hold(true);
    g_overview_screen.play(HELGA_PARTY);
    g_leds.playAlertPattern(LED_PATTERN_RAINBOW_FAST, 0);

    _ensureBanner();
    if (_banner) {
        lv_obj_clear_flag((lv_obj_t*)_banner, LV_OBJ_FLAG_HIDDEN);
    }
    if (OVERVIEW_TITLE) {
        lv_obj_add_flag(OVERVIEW_TITLE, LV_OBJ_FLAG_HIDDEN);
    }
    _refreshColors();
}

bool PartyService::handleMenuButton(EventId event_id) {
    if (!_session.consumeMenuExitButton(event_id)) return false;
    _teardownEffects(true);
    return true;
}

bool PartyService::consumeMenuExitFollowup(EventId event_id) {
    return _session.consumeMenuExitFollowup(event_id);
}

void PartyService::stop(bool arm_cooldown) {
    _end(arm_cooldown);
}

void PartyService::_end(bool set_cooldown) {
    if (!_session.stop()) return;
    _teardownEffects(set_cooldown);
}

void PartyService::_teardownEffects(bool set_cooldown) {
    g_leds.playAlertPattern(LED_PATTERN_OFF, 0);
    g_vibe.stop();
    g_display.clearIconTint();
    g_overview_screen.hold(false);
    g_overview_screen.play(HELGA_IDLE);

    if (_banner) {
        lv_obj_add_flag((lv_obj_t*)_banner, LV_OBJ_FLAG_HIDDEN);
    }
    if (OVERVIEW_TITLE) {
        lv_obj_clear_flag(OVERVIEW_TITLE, LV_OBJ_FLAG_HIDDEN);
    }

    if (set_cooldown) {
        _cooldown.arm(millis());
    }
}

void PartyService::tick() {
    if (!_session.active()) return;
    uint32_t now = millis();

    if (_navigation.shouldDismiss(_currentPartyScreen())) {
        _end(true);
        return;
    }

    if (_session.expired(now)) {
        _end(false);
        return;
    }

    if (now - _last_vibe_ms >= VIBE_INTERVAL_MS) {
        _last_vibe_ms = now;
        g_vibe.play(PARTY_VIBE);
    }
    if (now - _last_text_ms >= TEXT_INTERVAL_MS) {
        _last_text_ms = now;
        _refreshColors();
    }
    if (now - _last_awake_ms >= AWAKE_INTERVAL_MS) {
        _last_awake_ms = now;
        _bus->post(EV_UI_ACTIVITY);
    }
}
