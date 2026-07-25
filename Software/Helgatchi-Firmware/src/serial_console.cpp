#include "serial_console.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "event_payload.h"
#include "led_service.h"
#include "vibe_service.h"
#include "hal.h"
#include "power_manager.h"
#include "power_menu_screen.h"
#include "ui_controller.h"
#include "alerts_service.h"
#include "scan_service.h"
#include "vendor_lookup.h"
#include "rules_service.h"
#include "party_service.h"
#include "admin_service.h"
#include "version.h"
#include <Arduino.h>
#include <FastLED.h>
#include <lvgl.h>
#include <stdlib.h>
#include <string.h>

static int _resolveLedId(const char* s);   // defined lower; used by _cmdAdmin

// Trim leading/trailing ASCII whitespace in place (internal whitespace kept).
// Matches the password normalization in scripts/build_admin_secret.py so the
// on-device PBKDF2 input equals the build-time input byte-for-byte.
static char* _trimWs(char* s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t') s++;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    return s;
}

// ---------------------------------------------------------------------------
// Human-readable formatters for stats output. Each writes into a caller-
// supplied buffer so multiple values can appear in the same printf without
// stepping on each other.
// ---------------------------------------------------------------------------

static const char* fmt_bytes(char* out, size_t out_sz, uint32_t bytes) {
    if      (bytes >= (1U << 20)) snprintf(out, out_sz, "%.1f MB", bytes / (double)(1U << 20));
    else if (bytes >= (1U << 10)) snprintf(out, out_sz, "%.1f KB", bytes / (double)(1U << 10));
    else                          snprintf(out, out_sz, "%u B",    (unsigned)bytes);
    return out;
}

static const char* fmt_hz(char* out, size_t out_sz, uint32_t hz) {
    if      (hz >= 1000000) snprintf(out, out_sz, "%u MHz", (unsigned)(hz / 1000000));
    else if (hz >= 1000)    snprintf(out, out_sz, "%u kHz", (unsigned)(hz / 1000));
    else                    snprintf(out, out_sz, "%u Hz",  (unsigned)hz);
    return out;
}

static const char* fmt_uptime(char* out, size_t out_sz, uint32_t ms) {
    uint32_t s = ms / 1000;
    if      (s < 60)   snprintf(out, out_sz, "%u.%03us",       (unsigned)s,               (unsigned)(ms % 1000));
    else if (s < 3600) snprintf(out, out_sz, "%um %02us",      (unsigned)(s/60),          (unsigned)(s%60));
    else               snprintf(out, out_sz, "%uh %02um %02us",(unsigned)(s/3600),
                                              (unsigned)((s/60)%60), (unsigned)(s%60));
    return out;
}

SerialConsole g_console;

// Keep in sync with SettingsKey enum order. The array size is intentionally
// inferred from the initializers — if you add an enum entry but forget a name
// here, the static_assert below fails at compile time (instead of nullptr-deref
// at runtime, which is what happened when SKEY_NO_SLEEP_WHILE_CHARGING was added).
static const char* const s_key_name[] = {
    "SCREEN_BRIGHTNESS",       // 0
    "LED_BRIGHTNESS",          // 1
    "SCAN_MODE",               // 2
    "SCAN_ACTIVE",             // 3
    "PERF_MODE",               // 4
    "ALERT_WAKE_SCREEN",       // 5
    "ALERT_VIBRATION",         // 6
    "ALERT_LED",               // 7
    "ALERT_FOCUS",             // 8
    "SLEEP_WHILE_USB",         // 9
    "VSENSE_5V_DIVIDER",       // 10
    "DEBUG_SERIAL",            // 11
    "DEBUG_LEVEL",             // 12
    "DEBUG_SLEEP_W_SERIAL",    // 13
    "SLEEP_WHILE_CHARGING",    // 14
    "INTERACTIVE_TIMEOUT_S",   // 15
    "SLEEP_DURATION_S",        // 16
    "SCAN_DURATION_S",         // 17
    "TUTORIAL_SHOWN",          // 18
    "IGNORE_RANDOMIZED_MACS",  // 19
    "HUNT_VIBRATION",          // 20
    "DEBUG_ENABLED",           // 21
};
static_assert(sizeof(s_key_name) / sizeof(s_key_name[0]) == SKEY_COUNT,
              "s_key_name is out of sync with SettingsKey");

// Resolve a `setting set` key token to a SettingsKey index. Accepts either a
// numeric id (0..SKEY_COUNT-1) or a case-insensitive setting name, with or
// without the SKEY_ prefix (so "led_brightness", "LED_BRIGHTNESS" and
// "SKEY_LED_BRIGHTNESS" all work). Returns SKEY_COUNT on no match.
static uint8_t resolveSettingKey(const char* s) {
    if (!s || !*s) return SKEY_COUNT;

    bool numeric = true;
    for (const char* p = s; *p; ++p) {
        if (!isdigit((unsigned char)*p)) { numeric = false; break; }
    }
    if (numeric) {
        long id = atol(s);
        return (id >= 0 && id < SKEY_COUNT) ? (uint8_t)id : SKEY_COUNT;
    }

    if (strncasecmp(s, "SKEY_", 5) == 0) s += 5;
    for (uint8_t k = 0; k < SKEY_COUNT; k++) {
        if (strcasecmp(s, s_key_name[k]) == 0) return k;
    }
    return SKEY_COUNT;
}

// LED + haptic pattern name registries are owned by their respective
// services now (led_service.cpp / vibe_service.cpp). RulesService and this
// console share them via ledPatternName / ledPatternByName / etc.

// ---------------------------------------------------------------------------

void SerialConsole::begin(EventBus& bus) {
    _bus = &bus;
    _pos = 0;
}

void SerialConsole::tick() {
    // `(bool)Serial` on Arduino-ESP32 USB CDC briefly drops false during
    // SOF/transfer pauses even when the terminal is attached. Apply a 2-second
    // hysteresis so mid-typing pauses don't trigger the disconnect path —
    // which used to wipe _pos and fragment long commands the moment the user
    // paused. The reset still happens for genuine disconnects (>2 s gap).
    static constexpr uint32_t DISCONNECT_GRACE_MS = 2000;
    const uint32_t now = millis();
    bool connected = (bool)Serial;
    if (connected) {
        _last_seen_connected_ms = now;
    } else if (_last_seen_connected_ms != 0 &&
               (now - _last_seen_connected_ms) < DISCONNECT_GRACE_MS) {
        connected = true;
    }

    // Only reset the input buffer on a real DISCONNECT→CONNECT edge so a
    // mid-typing CDC blip doesn't lose the partial command.
    if (connected && !_was_connected) {
        _pos = 0;
    }
    _was_connected = connected;

    if (!connected) return;

    while (Serial.available()) {
        char c = (char)Serial.read();

        if (c == '\r') continue;

        if (c == '\n') {
            Serial.println();
            _buf[_pos] = '\0';
            if (_pos > 0) {
                _dispatch(_buf);
                Serial.println();
            }
            _pos = 0;

        } else if (c == 0x7F || c == '\b') {
            if (_pos > 0) {
                _pos--;
                Serial.print("\b \b");  // overwrite with space then move back
            }

        } else if (_pos < BUF_LEN - 1) {
            _buf[_pos++] = c;
            // Mask the secret on `admin unlock <password>`: once the line is past
            // the "admin unlock " prefix, echo '*' instead of the typed char so
            // the password never appears on the terminal. (The real bytes still
            // land in _buf for dispatch.)
            static const char PW_PREFIX[] = "admin unlock ";
            const size_t PLEN = sizeof(PW_PREFIX) - 1;
            const bool mask = (_pos > PLEN) && strncasecmp(_buf, PW_PREFIX, PLEN) == 0;
            Serial.print(mask ? '*' : c);  // local echo
            _bus->post(EV_UI_ACTIVITY);  // reset interactive sleep timer
        }
    }
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

void SerialConsole::_dispatch(char* line) {
    char* verb = strtok(line, " ");
    char* rest = strtok(nullptr, "");
    if (!verb) return;

    if      (strcmp(verb, "help")     == 0) _cmdHelp();
    else if (strcmp(verb, "setting")  == 0) _cmdSetting(rest);
    else if (strcmp(verb, "alert")    == 0) _cmdAlert(rest);
    else if (strcmp(verb, "led")      == 0) _cmdLed(rest);
    else if (strcmp(verb, "vibe")     == 0) _cmdVibe(rest);
    else if (strcmp(verb, "rule")     == 0) _cmdRule(rest);
    else if (strcmp(verb, "party")    == 0) _cmdParty(rest);
    else if (strcmp(verb, "admin")    == 0) _cmdAdmin(rest);
    else if (strcmp(verb, "scan")     == 0) _cmdScan(rest);
    else if (strcmp(verb, "vendor")   == 0) _cmdVendor(rest);
    else if (strcmp(verb, "power")    == 0) _cmdPower(rest);
    else if (strcmp(verb, "bus")      == 0) _cmdBus(rest);
    else if (strcmp(verb, "stats")    == 0) _cmdStats();
    else if (strcmp(verb, "battery")  == 0) _cmdBattery();
    else if (strcmp(verb, "selftest") == 0) _cmdSelftest();
    else if (strcmp(verb, "ver")      == 0) _cmdVer();
    else if (strcmp(verb, "update")   == 0) _cmdUpdate();
    else if (strcmp(verb, "webinfo")  == 0) _cmdWebinfo();   // hidden: web-app bootstrap
    else Serial.printf("unknown command '%s'  (try 'help')\n", verb);
}

void SerialConsole::_cmdHelp() {
    Serial.println("commands (run a verb without args for its subcommand list):");
    Serial.println("  admin <subcmd>              admin crowd-control       (unlock / lock / party / msg /");
    Serial.println("                                                         led / beacon / stopall / menu)");
    Serial.println("  alert <subcmd>              active alert store        (list / raise / ack / clear)");
    Serial.println("  battery                     voltage / pct / charging state");
    Serial.println("  bus post <event_id>         post an event by numeric id");
    Serial.println("  led <subcmd>                LED pattern control       (list / play / off / bright)");
    Serial.println("  party <subcmd>              party mode                (on [secs] / off)");
    Serial.println("  power <subcmd>              device power ops          (sleep / sleepscreen / reboot / shipping / wipe / off)");
    Serial.println("  rule <subcmd>               rules engine              (list / show / create / add / rm / delete /");
    Serial.println("                                                         enable / disable / reload / stats)");
    Serial.println("  scan <subcmd>               scan-result ring          (list / inject / clear)");
    Serial.println("  selftest                    GPIO short / load detect");
    Serial.println("  setting <subcmd>            settings store            (list / set / save / reset)");
    Serial.println("  stats                       chip / memory / display info");
    Serial.println("  vendor <subcmd>             IEEE / BT SIG lookups     (stats / oui / mfg / search)");
    Serial.println("  vibe <subcmd>               haptic motor              (list / play / off)");
}

// `setting <subcmd>` — settings store ops.
void SerialConsole::_cmdSetting(char* args) {
    if (!args) {
        Serial.println("setting: settings store");
        Serial.println("  setting list                  print every setting + current value");
        Serial.println("  setting set <id|name> <value> write one setting by id or name");
        Serial.println("  setting save                  persist current values to NVS");
        Serial.println("  setting reset                 restore factory defaults");
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "list") == 0) {
        Serial.println(" id  name                    value");
        Serial.println("---  ----------------------  -----");
        for (uint8_t k = 0; k < SKEY_COUNT; k++) {
            Serial.printf("%3u  %-22s  %lu\n",
                          k, s_key_name[k],
                          (unsigned long)g_settings.get((SettingsKey)k));
        }
        return;
    }

    if (sub && strcasecmp(sub, "set") == 0) {
        if (!rest) { Serial.println("usage: setting set <id|name> <value>"); return; }
        char* k_str = strtok(rest, " ");
        char* v_str = strtok(nullptr, " ");
        if (!k_str || !v_str) { Serial.println("usage: setting set <id|name> <value>"); return; }
        uint8_t key = resolveSettingKey(k_str);
        if (key >= SKEY_COUNT) {
            Serial.printf("bad setting '%s' (use id 0-%u or a name from 'setting list')\n",
                          k_str, SKEY_COUNT - 1);
            return;
        }
        uint32_t val = (uint32_t)atol(v_str);
        EventPayload p{};
        p.settings_set.key   = (SettingsKey)key;
        p.settings_set.value = val;
        _bus->post(CMD_SETTINGS_SET, p);
        Serial.printf("OK: [%u] %s = %lu\n", key, s_key_name[key], (unsigned long)val);
        return;
    }

    if (sub && strcasecmp(sub, "save") == 0) {
        _bus->post(CMD_SETTINGS_SAVE);
        Serial.println("OK: settings saved to NVS");
        return;
    }

    if (sub && strcasecmp(sub, "reset") == 0) {
        _bus->post(CMD_SETTINGS_RESET_DEFAULTS);
        Serial.println("OK: settings reset to factory defaults");
        return;
    }

    Serial.printf("unknown subcommand 'setting %s'  (try 'setting')\n", sub ? sub : "");
}

// `ver` — one machine-readable line for the web companion to parse on connect.
void SerialConsole::_cmdVer() {
    Serial.printf("{\"fw\":\"%s\",\"hw\":\"%s\",\"chip\":\"ESP32-S3\",\"game\":\"%s\",\"ui\":\"%s\"}\n",
                  FW_VERSION_STR, HW_REV_STR, GAME_VERSION_STR, UI_VERSION_STR);
}

// `update` — paint the updating screen, then ack. The web flasher sends this,
// waits for the ack, and only then resets the device into the bootloader so the
// "updating" screen stays on the panel through the flash.
void SerialConsole::_cmdUpdate() {
    g_ui.showUpdatingScreen();
    Serial.println("{\"updating\":true}");
}

// `webinfo` (hidden) — one JSON line with everything the web companion needs on
// connect: version, the LED + vibe pattern registries, and all rules. Folds
// what used to be four commands (ver / led list / vibe list / rule dump) into a
// single round-trip; the web app mutes it from the console view.
void SerialConsole::_cmdWebinfo() {
    JsonDocument doc;
    doc["fw"]   = FW_VERSION_STR;
    doc["hw"]   = HW_REV_STR;
    doc["chip"] = "ESP32-S3";
    doc["game"] = GAME_VERSION_STR;
    doc["ui"]   = UI_VERSION_STR;

    JsonArray led = doc["led"].to<JsonArray>();
    ledPatternForEach([](LedPatternId, const char* name, void* user) {
        static_cast<JsonArray*>(user)->add(name);
    }, &led);

    JsonArray vibe = doc["vibe"].to<JsonArray>();
    for (uint8_t i = 0; i < HAPTIC_PATTERN_COUNT; i++) vibe.add(vibePatternName((HapticPatternId)i));

    doc["admin_unlocked"]       = g_admin.unlocked();
    doc["admin_default_secret"] = g_admin.secretIsDefault();   // true = insecure dev build

    g_rules.toJson(doc["rules"].to<JsonArray>());

    serializeJson(doc, Serial);   // compact — one line
    Serial.println();
}

void SerialConsole::_cmdBus(char* args) {
    if (!args) { Serial.println("usage: bus post <event_id>"); return; }
    char* sub = strtok(args, " ");
    if (!sub || strcmp(sub, "post") != 0) {
        Serial.println("usage: bus post <event_id>");
        return;
    }
    char* id_str = strtok(nullptr, " ");
    if (!id_str) { Serial.println("usage: bus post <event_id>"); return; }

    EventId id = (EventId)atoi(id_str);
    _bus->post(id);
    Serial.printf("OK: posted event %u\n", (unsigned)id);
}

void SerialConsole::_cmdStats() {
    char b1[24], b2[24], b3[24];

    Serial.printf("uptime:     %s\n", fmt_uptime(b1, sizeof(b1), millis()));

    // Chip
    Serial.printf("chip:       %s rev %u, %u core(s) @ %u MHz, IDF %s\n",
                  ESP.getChipModel(),
                  (unsigned)ESP.getChipRevision(),
                  (unsigned)ESP.getChipCores(),
                  (unsigned)ESP.getCpuFreqMHz(),
                  ESP.getSdkVersion());

    // Flash + sketch occupancy. Sketch has no "min seen" — it's flashed
    // once, the value can't change at runtime.
    Serial.printf("flash:      %s @ %s\n",
                  fmt_bytes(b1, sizeof(b1), ESP.getFlashChipSize()),
                  fmt_hz   (b2, sizeof(b2), ESP.getFlashChipSpeed()));
    Serial.printf("sketch:     %s / %s used (%s unused in app partition)\n",
                  fmt_bytes(b1, sizeof(b1), ESP.getSketchSize()),
                  fmt_bytes(b2, sizeof(b2), ESP.getSketchSize() + ESP.getFreeSketchSpace()),
                  fmt_bytes(b3, sizeof(b3), ESP.getFreeSketchSpace()));

    // Internal SRAM heap — consumption against capacity.
    {
        const uint32_t total = ESP.getHeapSize();
        const uint32_t free  = ESP.getFreeHeap();
        Serial.printf("heap:       %s / %s used\n",
                      fmt_bytes(b1, sizeof(b1), total - free),
                      fmt_bytes(b2, sizeof(b2), total));
    }

    // PSRAM (0 if chip variant has none, or PSRAM init failed)
    const size_t psram_total = ESP.getPsramSize();
    if (psram_total) {
        const uint32_t free = ESP.getFreePsram();
        Serial.printf("psram:      %s / %s used\n",
                      fmt_bytes(b1, sizeof(b1), (uint32_t)psram_total - free),
                      fmt_bytes(b2, sizeof(b2), (uint32_t)psram_total));
    } else {
        Serial.println("psram:      absent");
    }

    // LVGL builtin allocator pool (LV_MEM_SIZE): consumption against capacity,
    // plus fragmentation. frag_pct rising means heavy churn (rare for static
    // UIs).
    lv_mem_monitor_t lv_mon;
    lv_mem_monitor(&lv_mon);
    Serial.printf("lv_mem:     %s / %s used (frag %u%%)\n",
                  fmt_bytes(b1, sizeof(b1), lv_mon.total_size - lv_mon.free_size),
                  fmt_bytes(b2, sizeof(b2), lv_mon.total_size),
                  (unsigned)lv_mon.frag_pct);

    // Display info. Strip count comes from ground-truth flush_cb counter;
    // frame rate is strips/2 in current 120-row PARTIAL setup (1-2 strips
    // per dirty frame). Panel internal scan rate is fixed by ST7789 default
    // register values — see Architecture notes for why this isn't host-
    // controllable on SPI panels.
    uint32_t flushes, elapsed_ms;
    g_ui.getDisplayStats(flushes, elapsed_ms);
    Serial.printf("display:    SPI @ 80 MHz, panel scan ~60 Hz (ST7789 default)\n");
    if (elapsed_ms > 0) {
        const uint32_t strips_per_s = flushes * 1000 / elapsed_ms;
        Serial.printf("flushes:    %u strips in %s (%u strips/s, ~%u fps)\n",
                      (unsigned)flushes,
                      fmt_uptime(b1, sizeof(b1), elapsed_ms),
                      (unsigned)strips_per_s,
                      (unsigned)(strips_per_s / 2));
    } else {
        Serial.println("flushes:    (call stats again after some activity)");
    }

    Serial.printf("bus drops:  %u\n", g_bus.droppedCount());
}

// `led <subcmd>` — LED pattern control.
void SerialConsole::_cmdLed(char* args) {
    if (!args) {
        Serial.println("led: alert LED pattern control");
        Serial.println("  led list                      list pattern names + ids");
        Serial.println("  led play <name|id> [ms]       play one pattern (ms=0 or omitted = until preempted)");
        Serial.println("  led off                       clear the alert layer (returns to ambient)");
        Serial.println("  led bright <0-255>            override FastLED brightness (debug)");
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "list") == 0) {
        Serial.println(" id  name");
        Serial.println("---  --------------");
        ledPatternForEach([](LedPatternId id, const char* name, void*) {
            Serial.printf("%3u  %s\n", (unsigned)id, name);
        }, nullptr);
        return;
    }

    if (sub && strcasecmp(sub, "play") == 0) {
        if (!rest) { Serial.println("usage: led play <name|id> [ms]"); return; }
        char* arg1 = strtok(rest, " ");
        char* arg2 = strtok(nullptr, " ");
        LedPatternId pat = ledPatternByName(arg1);
        if (pat == LED_PATTERN_COUNT) {
            char* end = nullptr;
            long n = strtol(arg1, &end, 10);
            if (end != arg1 && *end == '\0' && n >= 0 && n < LED_PATTERN_COUNT) {
                pat = (LedPatternId)n;
            }
        }
        if (pat == LED_PATTERN_COUNT) {
            Serial.printf("unknown pattern '%s'  (try 'led list')\n", arg1);
            return;
        }
        uint32_t duration_ms = arg2 ? (uint32_t)atol(arg2) : 0;
        g_leds.playAlertPattern(pat, duration_ms);
        if (duration_ms > 0) Serial.printf("OK: %s for %lu ms\n", ledPatternName(pat), (unsigned long)duration_ms);
        else                 Serial.printf("OK: %s (until preempted or 'led off')\n", ledPatternName(pat));
        return;
    }

    if (sub && strcasecmp(sub, "off") == 0) {
        g_leds.playAlertPattern(LED_PATTERN_OFF, 0);
        Serial.println("OK: alert layer cleared");
        return;
    }

    if (sub && (strcasecmp(sub, "bright") == 0 || strcasecmp(sub, "brightness") == 0)) {
        if (!rest) { Serial.println("usage: led bright <0-255>"); return; }
        int n = atoi(rest);
        if (n < 0)   n = 0;
        if (n > 255) n = 255;
        FastLED.setBrightness((uint8_t)n);
        FastLED.show();
        Serial.printf("OK: FastLED brightness = %d (reverts on settings change / reboot)\n", n);
        return;
    }

    Serial.printf("unknown subcommand 'led %s'  (try 'led')\n", sub ? sub : "");
}

// `vibe <subcmd>` — haptic pattern control.
void SerialConsole::_cmdVibe(char* args) {
    if (!args) {
        Serial.println("vibe: haptic motor control");
        Serial.println("  vibe list                     list pattern names + ids");
        Serial.println("  vibe play <name|id>           play one pattern");
        Serial.println("  vibe off                      stop the motor immediately");
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "list") == 0) {
        Serial.println(" id  name");
        Serial.println("---  -----------");
        for (uint8_t i = 0; i < HAPTIC_PATTERN_COUNT; i++) {
            Serial.printf("%3u  %s\n", i, vibePatternName((HapticPatternId)i));
        }
        return;
    }

    if (sub && strcasecmp(sub, "play") == 0) {
        if (!rest) { Serial.println("usage: vibe play <name|id>"); return; }
        char* arg1 = strtok(rest, " ");
        HapticPatternId pat = vibePatternByName(arg1);
        if (pat == HAPTIC_PATTERN_COUNT) {
            char* end = nullptr;
            long n = strtol(arg1, &end, 10);
            if (end != arg1 && *end == '\0' && n >= 0 && n < HAPTIC_PATTERN_COUNT) {
                pat = (HapticPatternId)n;
            }
        }
        if (pat == HAPTIC_PATTERN_COUNT) {
            Serial.printf("unknown pattern '%s'  (try 'vibe list')\n", arg1);
            return;
        }
        g_vibe.play(pat);
        Serial.printf("OK: %s\n", vibePatternName(pat));
        return;
    }

    if (sub && strcasecmp(sub, "off") == 0) {
        g_vibe.play(HAPTIC_OFF);
        Serial.println("OK: motor off");
        return;
    }

    Serial.printf("unknown subcommand 'vibe %s'  (try 'vibe')\n", sub ? sub : "");
}

// `party <subcmd>` — party mode (rainbow LEDs + haptics + dance anim + banner).
void SerialConsole::_cmdParty(char* args) {
    if (!args) {
        if (g_party.active())
            Serial.printf("party: ACTIVE (%lus left)\n",
                          (unsigned long)((g_party.remainingMs() + 999) / 1000));
        else
            Serial.println("party: off");
        Serial.println("  party on [secs]               start party mode (default 20s; re-run extends)");
        Serial.println("  party off                     stop immediately");
        Serial.println("  (long-press-back on the device also exits party mode)");
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "on") == 0) {
        uint32_t secs = rest ? (uint32_t)atol(rest) : 0;
        uint32_t ms   = secs ? secs * 1000UL : PartyService::DEFAULT_DURATION_MS;
        g_party.start(ms);
        Serial.printf("OK: party for %lus\n", (unsigned long)(ms / 1000));
        return;
    }

    if (sub && strcasecmp(sub, "off") == 0) {
        g_party.stop();
        Serial.println("OK: party off");
        return;
    }

    Serial.printf("unknown subcommand 'party %s'  (try 'party')\n", sub ? sub : "");
}

// `admin <subcmd>` — HMAC-signed BLE crowd control. Receiving/acting on commands
// needs no unlock (all devices obey a valid frame); SENDING requires `unlock`.
void SerialConsole::_cmdAdmin(char* args) {
    if (!args) {
        Serial.printf("admin: %s%s\n",
                      g_admin.unlocked() ? "UNLOCKED (can send)" : "locked (receive-only)",
                      g_admin.broadcasting() ? "  [broadcasting]" : "");
        if (g_admin.secretIsDefault())
            Serial.println("  WARNING: built with DEV-DEFAULT secrets — not for release");
        Serial.println("  admin unlock <password>       enable send mode (persists in NVS)");
        Serial.println("  admin lock                    disable send mode");
        Serial.println("  admin party on [secs]         broadcast: start party");
        Serial.println("  admin party off               broadcast: stop party");
        Serial.println("  admin msg <idx> [secs]        broadcast: show predefined message");
        Serial.println("  admin led <name|id> [secs]    broadcast: run an LED pattern");
        Serial.println("  admin beacon [secs]           broadcast: everyone name-beacons");
        Serial.println("  admin stopall                 broadcast: tell receivers to cancel effects");
        Serial.println("  admin stopbroadcast           stop THIS device's advert (sends nothing)");
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "unlock") == 0) {
        char* pw = _trimWs(rest);
        if (!pw || !*pw) { Serial.println("usage: admin unlock <password>"); return; }
        if (g_admin.unlock(pw)) Serial.println("OK: admin unlocked");
        else                    Serial.println("ERR: wrong password");
        return;
    }
    if (sub && strcasecmp(sub, "lock") == 0) {
        g_admin.lock();
        Serial.println("OK: admin locked");
        return;
    }

    // Everything below broadcasts — requires unlock.
    if (!g_admin.unlocked()) {
        Serial.println("ERR: locked  (run 'admin unlock <password>' first)");
        return;
    }

    if (sub && strcasecmp(sub, "party") == 0) {
        char* onoff  = strtok(rest, " ");
        char* secs_s = strtok(nullptr, "");
        if (onoff && strcasecmp(onoff, "on") == 0) {
            uint16_t secs = secs_s ? (uint16_t)atoi(secs_s) : 20;
            g_admin.broadcast(ADMIN_CMD_PARTY_START, 0, secs);
            Serial.printf("OK: broadcast party on (%us)\n", secs);
        } else if (onoff && strcasecmp(onoff, "off") == 0) {
            g_admin.broadcast(ADMIN_CMD_PARTY_STOP, 0, 0);
            Serial.println("OK: broadcast party off");
        } else {
            Serial.println("usage: admin party on [secs] | admin party off");
        }
        return;
    }
    if (sub && strcasecmp(sub, "msg") == 0) {
        char* idx_s  = strtok(rest, " ");
        char* secs_s = strtok(nullptr, "");
        if (!idx_s) {
            // Bare `admin msg` — list the predefined messages and their indexes.
            Serial.println("admin msg <idx> [secs] — broadcast a predefined message:");
            for (uint8_t i = 0; i < AdminService::messageCount(); i++)
                Serial.printf("  %u = \"%s\"\n", i, AdminService::messageText(i));
            return;
        }
        int idx = atoi(idx_s);
        if (idx < 0 || idx >= AdminService::messageCount()) {
            Serial.printf("bad msg idx (0-%u)  (run 'admin msg' to list)\n",
                          (unsigned)(AdminService::messageCount() - 1));
            return;
        }
        uint16_t secs = secs_s ? (uint16_t)atoi(secs_s) : 10;
        g_admin.broadcast(ADMIN_CMD_MESSAGE, (uint8_t)idx, secs);
        Serial.printf("OK: broadcast msg %d (\"%s\", %us)\n",
                      idx, AdminService::messageText((uint8_t)idx), secs);
        return;
    }
    if (sub && strcasecmp(sub, "led") == 0) {
        char* name_s = strtok(rest, " ");
        char* secs_s = strtok(nullptr, "");
        if (!name_s) { Serial.println("usage: admin led <name|id> [secs]  (see 'led list')"); return; }
        int id = _resolveLedId(name_s);
        if (id < 0) { Serial.printf("bad led '%s' (see 'led list')\n", name_s); return; }
        uint16_t secs = secs_s ? (uint16_t)atoi(secs_s) : 15;
        g_admin.broadcast(ADMIN_CMD_LED, (uint8_t)id, secs);
        Serial.printf("OK: broadcast led %s (%us)\n", ledPatternName((LedPatternId)id), secs);
        return;
    }
    if (sub && strcasecmp(sub, "beacon") == 0) {
        uint16_t secs = rest ? (uint16_t)atoi(rest) : 30;
        g_admin.broadcast(ADMIN_CMD_BEACON, 0, secs);
        Serial.printf("OK: broadcast beacon (%us)\n", secs);
        return;
    }
    if (sub && strcasecmp(sub, "stopall") == 0) {
        g_admin.broadcast(ADMIN_CMD_STOP_ALL, 0, 0);
        Serial.println("OK: broadcast stop-all");
        return;
    }
    if (sub && strcasecmp(sub, "stopbroadcast") == 0) {
        // Stop THIS device's own advert — purely local. Distinct from `stopall`,
        // which TRANSMITS a command telling receivers to cancel their effects.
        if (!g_admin.broadcasting()) { Serial.println("admin: not broadcasting"); return; }
        g_admin.stopBroadcast();
        Serial.println("OK: stopped broadcasting");
        return;
    }

    Serial.printf("unknown subcommand 'admin %s'  (try 'admin')\n", sub ? sub : "");
}

// ---------------------------------------------------------------------------
// `selftest` — GPIO short-detection scan
//
// For each testable digital pin: briefly reconfigure as INPUT_PULLUP and
// INPUT_PULLDOWN, read the line in each state. Compare:
//   HIGH/LOW  → FLOATING (internal pull dominates — nothing strong external)
//   LOW/LOW   → external load to GND (or hard short)
//   HIGH/HIGH → external load to VCC (or hard short)
//
// For VSENSE: read analog mV directly. 0 mV usually means battery missing or
// divider open; rail-pegged means short to the input side of the divider.
//
// What's NOT tested and why:
//   • SPI_DC, SPI_SCK, SPI_RST, SPI_MOSI, SPI_CS — the display SPI bus runs
//     continuously and we can't restore the IO_MUX→SPI routing reliably from
//     a one-shot Arduino-API call. The display's own init at boot is the
//     implicit test for these (if it failed, you'd see no UI).
//   • LED_DATA — tested, but FastLED's RMT routing isn't cleanly restorable
//     from a pinMode() switch. Recommend reboot after selftest if LEDs
//     don't respond. Or run `led <pattern>` to verify they do.
//
// Don't press buttons during the scan — a held button looks identical to a
// short to GND on the matrix lines.
// ---------------------------------------------------------------------------

static void _selftestDigitalPin(uint8_t gpio, const char* label) {
    pinMode(gpio, INPUT_PULLUP);
    delayMicroseconds(50);
    int hi = digitalRead(gpio);
    pinMode(gpio, INPUT_PULLDOWN);
    delayMicroseconds(50);
    int lo = digitalRead(gpio);

    Serial.printf(" %3u    %-19s  ", gpio, label);
    if      (hi == HIGH && lo == LOW)  Serial.println("FLOATING     OK");
    else if (hi == LOW  && lo == LOW)  Serial.println("PULLED_LOW   external load to GND / SHORT_GND?");
    else if (hi == HIGH && lo == HIGH) Serial.println("PULLED_HIGH  external load to VCC / SHORT_VCC?");
    else                               Serial.println("ANOMALOUS    inverted — pin actively driven?");
}

void SerialConsole::_cmdSelftest() {
    Serial.println("=== GPIO self-test ===");
    Serial.println("Skipped: SPI bus pins (SCK/MOSI/CS/DC/RST). Display init at boot is their implicit test.");
    Serial.println("Don't press buttons during the scan.");
    Serial.println();
    Serial.println(" GPIO   Pin                  Result");
    Serial.println(" ----   -------------------  -------------------------------------------");

    // --- Analog: VSENSE ----------------------------------------------------
    analogSetAttenuation(ADC_11db);
    int32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VSENSE);
    int vmv = sum / 8;
    Serial.printf(" %3u    %-19s  ADC=%-4d mV  ", PIN_VSENSE, "VSENSE   (GPIO5)", vmv);
    if      (vmv < 20)   Serial.println("(0 V — battery missing or divider open)");
    else if (vmv > 3200) Serial.println("(rail — short to VCC / no divider)");
    else if (g_settings.getBool(SKEY_VSENSE_5V_DIVIDER)) {
        // R4 populated: VBATT line = 3·VSENSE − V5 rail (see power_manager.h).
        bool charging = vmv > PM_R4_CHARGING_MV;
        int32_t vb = pmR4VbattLineMv((uint16_t)vmv, charging ? g_power.chargeRailMv()
                                                             : PM_R4_DISCHG_RAIL_MV);
        if (charging) vb -= PM_R4_CHARGE_LIFT_MV;   // cell estimate
        Serial.printf ("(vbatt~%d mV via 3-resistor +R4 divider, %s)\n",
                       vb, charging ? "charging (5V rail sensed)" : "on battery");
    }
    else                 Serial.printf ("(vbatt~%d mV via /2 divider)\n", vmv * 2);

    // --- Digital: testable pins -------------------------------------------
    struct DigPin { uint8_t gpio; const char* label; };
    static const DigPin pins[] = {
        {PIN_LED_DATA, "LED_DATA (GPIO2)"},
        {PIN_SPI_BL,   "SPI_BL   (GPIO3)"},
        {PIN_VIBRATE,  "VIBRATE  (GPIO4)"},
        {PIN_BTN_1,    "BTN_1    (GPIO6)"},
        {PIN_BTN_2,    "BTN_2    (GPIO43)"},
    };
    for (const auto& p : pins) _selftestDigitalPin(p.gpio, p.label);

    // --- Restore peripheral routing ---------------------------------------
    // Backlight + motor: re-attach LEDC channels (the pinMode() switch above
    // detached the IO_MUX from the LEDC peripheral). After re-attach the
    // channel's last duty re-applies; we still wakeDisplay() to be explicit.
    ledcAttachPin(PIN_SPI_BL,  HAL_BL_LEDC_CH);
    ledcAttachPin(PIN_VIBRATE, HAL_VIBE_LEDC_CH);
    ledcWrite(HAL_VIBE_LEDC_CH, 0);
    g_hal.wakeDisplay();

    // Buttons: HAL's poll timer reads these every HAL_BTN_POLL_MS anyway, but
    // leave them in a sane state for the in-between reads.
    pinMode(PIN_BTN_1, INPUT_PULLUP);
    pinMode(PIN_BTN_2, INPUT_PULLUP);

    // LED_DATA: best-effort. FastLED's RMT routing is one-shot at addLeds()
    // and a pinMode() switch detaches it. Drive LOW so the data line idles
    // safely; LEDs may not light again until reboot.
    pinMode(PIN_LED_DATA, OUTPUT);
    digitalWrite(PIN_LED_DATA, LOW);

    Serial.println();
    Serial.println("Done. If LEDs are unresponsive after this command, reboot to restore FastLED's RMT routing.");
}

void SerialConsole::_cmdBattery() {
    // Take a fresh ADC reading so we report current voltage, not the cached
    // 30s-interval sample. The fresh raw% (curve, no smoothing) is shown
    // alongside the smoothed value the UI is currently using.
    uint16_t vsense   = g_hal.readVsenseMv();
    bool     usb      = g_hal.usbAttached();
    bool     ser      = (bool)Serial;
    bool     r4       = g_settings.getBool(SKEY_VSENSE_5V_DIVIDER);

    // Mirror PowerManager::_sampleBattery. R4 boards: VSENSE = (VBATT + V5)/3,
    // so vsense > threshold → +5V rail present → charging. VBATT line =
    // 3·vsense − rail; while charging the ~100mV +BATT lift is stripped for the
    // cell estimate. On USB, below threshold means no pack. Default boards are
    // always 2*vsense, charge state from USB data.
    bool     r4_missing  = r4 && usb && (vsense < PM_R4_CHARGING_MV);
    bool     r4_charging = r4 && (vsense > PM_R4_CHARGING_MV);
    uint16_t rail_mv  = r4_charging ? g_power.chargeRailMv() : PM_R4_DISCHG_RAIL_MV;
    uint16_t line_mv  = r4 ? pmR4VbattLineMv(vsense, rail_mv)
                           : (uint16_t)(vsense * 2);
    uint16_t vbatt_mv = r4_charging ? (uint16_t)(line_mv - PM_R4_CHARGE_LIFT_MV)
                                    : line_mv;
    uint8_t  raw_pct  = pmBattPctFromVsenseMv((uint16_t)(vbatt_mv / 2));

    Serial.printf("vsense:    %u mV  (raw ADC, post-divider)\n", vsense);
    Serial.printf("vbatt:     %u mV  (= %s)\n", vbatt_mv,
                  r4 ? (r4_charging ? "3*vsense - rail - lift" : "3*vsense - rail") : "vsense * 2");
    if (r4) Serial.printf("rail:      %u mV  (%s)\n", rail_mv,
                          !r4_charging ? "backfed, unplugged"
                          : g_power.chargeRailLearned() ? "learned (plug-in edge / NVS)"
                                                        : "fallback, never learned");
    Serial.printf("divider:   %s\n", r4 ? "3-resistor + R4 (+5V sense)" : "2-resistor (R4 cut)");
    if (r4) Serial.printf("sensed:    %s  (ADC %s %u mV threshold%s)\n",
                          r4_missing ? "NO PACK" : (r4_charging ? "CHARGING" : "on battery"),
                          vsense > PM_R4_CHARGING_MV ? ">" : "<=", PM_R4_CHARGING_MV,
                          r4_missing ? " while on USB" : "");
    Serial.printf("raw pct:   %u%%   (curve, no smoothing)\n",   raw_pct);

    uint8_t  last_pct = g_power.lastBatteryPct();
    uint16_t last_mv  = g_power.lastBatteryMv();
    Serial.print("last sent: ");
    if      (last_pct == 0xFF)              Serial.println("none yet");
    else if (last_pct == BATT_PCT_CHARGING) Serial.printf("CHARGING       (mv=%u)\n", last_mv);
    else if (last_pct == BATT_PCT_CHARGED)  Serial.printf("CHARGED        (mv=%u)\n", last_mv);
    else if (last_pct == BATT_PCT_MISSING)  Serial.println("MISSING/FAULT");
    else                                    Serial.printf("%u%%            (mv=%u, EMA-smoothed)\n",
                                                          last_pct, last_mv);

    Serial.print("charging:  ");
    if      (last_pct == BATT_PCT_CHARGED)  Serial.println("yes (full)");
    else if (last_pct == BATT_PCT_CHARGING) Serial.println("yes");
    else if (r4_charging)                   Serial.println("yes (fresh ADC read; sentinel lags ≤30s)");
    else if (usb)                           Serial.println("usb attached, not yet sampled as charging");
    else                                    Serial.println("no (discharging)");

    Serial.printf("USB:       %s\n", usb ? "attached" : "no");
    Serial.printf("Serial:    %s\n", ser ? "connected" : "no");
    Serial.printf("curve:     100%% @ %u mV vbatt  /  0%% @ %u mV vbatt  (LUT, %u points)\n",
                  PM_VSENSE_FULL_MV * 2, PM_VSENSE_DEAD_MV * 2, PM_BATT_CURVE_N);
}

// ---------------------------------------------------------------------------
// `alert` command — drive the AlertsService directly so we can develop / test
// the alert pipeline without the rules engine. Mirrors what the rules engine
// will do later: g_alerts.raise(...).
//
// Subcommands:
//   alert                          → list active alerts
//   alert list                     → list active alerts (explicit)
//   alert raise <title> [k=v]      → fire test alert. Keys: type, vibe, led, rssi, id
//   alert ack <id>                 → dismiss specific alert
//   alert clear                    → dismiss all alerts
//
// Title may contain spaces only if quoted in caller-land; the parser here
// treats the first whitespace-delimited token as the title. For multi-word
// alert titles over serial, use underscores (cosmetic — the rules engine
// will pass full strings at runtime).
// ---------------------------------------------------------------------------

static AlertType _parseAlertType(const char* s) {
    if (!s) return ALERT_SYSTEM;
    if (strcasecmp(s, "ble")     == 0) return ALERT_BLE;
    if (strcasecmp(s, "bt")      == 0) return ALERT_BLE;
    if (strcasecmp(s, "wifi")    == 0) return ALERT_WIFI;
    if (strcasecmp(s, "sys")     == 0) return ALERT_SYSTEM;
    if (strcasecmp(s, "system")  == 0) return ALERT_SYSTEM;
    if (strcasecmp(s, "batt")    == 0) return ALERT_BATTERY_LOW;
    if (strcasecmp(s, "battery") == 0) return ALERT_BATTERY_LOW;
    if (strcasecmp(s, "low")     == 0) return ALERT_BATTERY_LOW;
    return ALERT_SYSTEM;
}

static const char* _alertTypeName(AlertType t) {
    switch (t) {
        case ALERT_BLE:         return "ble";
        case ALERT_WIFI:        return "wifi";
        case ALERT_SYSTEM:      return "sys";
        case ALERT_BATTERY_LOW: return "batt";
        default:                return "?";
    }
}

// Resolve "name" or numeric id via the service registries. Returns -1 on
// miss; caller picks a default. Wrappers around vibePatternByName /
// ledPatternByName that also accept a numeric id as a power-user shortcut
// (rule files only support names).
static int _resolveVibeId(const char* s) {
    if (!s) return -1;
    HapticPatternId id = vibePatternByName(s);
    if (id != HAPTIC_PATTERN_COUNT) return (int)id;
    if (s[0] >= '0' && s[0] <= '9') {
        int n = atoi(s);
        if (n >= 0 && n < HAPTIC_PATTERN_COUNT) return n;
    }
    return -1;
}

static int _resolveLedId(const char* s) {
    if (!s) return -1;
    LedPatternId id = ledPatternByName(s);
    if (id != LED_PATTERN_COUNT) return (int)id;
    if (s[0] >= '0' && s[0] <= '9') {
        int n = atoi(s);
        if (n >= 0 && n < LED_PATTERN_COUNT) return n;
    }
    return -1;
}

// Splits an `alert raise` payload into a multi-word title and a k=v section.
// Everything up to (but not including) the first token containing `=` is the
// title; everything from that token onward is the k=v section. So
// `test alert 123 type=ble` → title="test alert 123", kv="type=ble".
//
// Mutates the input buffer in place. On return:
//   *title  → null-terminated multi-word title (trimmed of trailing space)
//   *kvs    → start of k=v section, or nullptr if none
// Returns true on success, false if the input is empty.
static bool _splitTitleAndKvs(char* input, char** title, char** kvs) {
    if (!input) return false;
    while (*input == ' ' || *input == '\t') input++;   // skip leading ws
    if (!*input) return false;

    // Scan forward for the first ` k=` or `k=` at start, where k contains
    // no spaces. Track the start of the current token so we can split there.
    char* tok_start = input;
    char* p         = input;
    while (*p) {
        if (*p == ' ' || *p == '\t') {
            tok_start = p + 1;
        } else if (*p == '=' && tok_start != input) {
            // Found a k=v token that's not the first; back up to its space
            // and split.
            char* split = tok_start - 1;
            *split = '\0';
            *title = input;
            *kvs   = tok_start;
            // Trim trailing whitespace on title.
            while (split > input && (*(split - 1) == ' ' || *(split - 1) == '\t')) {
                split--;
                *split = '\0';
            }
            return true;
        }
        p++;
    }

    // No k=v section found — the whole input is the title.
    // Trim trailing whitespace.
    char* end = input + strlen(input);
    while (end > input && (*(end - 1) == ' ' || *(end - 1) == '\t')) {
        end--;
        *end = '\0';
    }
    *title = input;
    *kvs   = nullptr;
    return true;
}

// `alert <subcmd>` — active-alert store ops.
void SerialConsole::_cmdAlert(char* args) {
    if (!args) {
        Serial.println("alert: active alert store");
        Serial.println("  alert list                    list active alerts");
        Serial.println("  alert raise <title...> [k=v]  fire an alert");
        Serial.println("                                k=v: type=ble|wifi|sys|batt  vibe=<name>");
        Serial.println("                                     led=<name>  rssi=<int>  id=<string>");
        Serial.println("  alert ack <id>                dismiss one alert by id");
        Serial.println("  alert clear                   dismiss all alerts");
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "list") == 0) {
        const uint8_t n = g_alerts.count();
        if (n == 0) { Serial.println("no active alerts"); return; }
        Serial.println(" id  type  count  age      rssi  title");
        Serial.println("---  ----  -----  -------  ----  -----");
        const uint32_t now = millis();
        for (uint8_t i = 0; i < n; i++) {
            const AlertRecord* r = g_alerts.get(i);
            if (!r) continue;
            const uint32_t age_s = (now - r->last_seen_ms) / 1000;
            char rssi_buf[8];
            if (r->rssi == INT8_MIN) snprintf(rssi_buf, sizeof(rssi_buf), "  --");
            else                     snprintf(rssi_buf, sizeof(rssi_buf), "%4d", (int)r->rssi);
            Serial.printf("%3u  %-4s  %5u  %4us    %s  %s",
                          (unsigned)r->id,
                          _alertTypeName(r->type),
                          (unsigned)r->seen_count,
                          (unsigned)age_s,
                          rssi_buf,
                          r->title);
            if (r->identifier[0]) Serial.printf("  [%s]", r->identifier);
            Serial.println();
        }
        return;
    }

    if (sub && strcasecmp(sub, "raise") == 0) {
        char* title = nullptr;
        char* kvs   = nullptr;
        if (!_splitTitleAndKvs(rest, &title, &kvs) || !title || !title[0]) {
            Serial.println("usage: alert raise <title...> [type=...] [vibe=...] [led=...] [rssi=N] [id=...]");
            return;
        }
        AlertType       type  = ALERT_SYSTEM;
        HapticPatternId vibe  = HAPTIC_TICK;
        LedPatternId    led   = LED_PATTERN_ALERT_DEFAULT;
        int8_t          rssi  = INT8_MIN;
        const char*     ident = nullptr;
        for (char* tok = strtok(kvs, " "); tok; tok = strtok(nullptr, " ")) {
            char* eq = strchr(tok, '=');
            if (!eq) { Serial.printf("ignoring '%s' (expected k=v)\n", tok); continue; }
            *eq = '\0';
            const char* k = tok;
            const char* v = eq + 1;
            if      (strcasecmp(k, "type") == 0) type = _parseAlertType(v);
            else if (strcasecmp(k, "vibe") == 0) {
                int id = _resolveVibeId(v);
                if (id < 0) { Serial.printf("unknown vibe '%s'  (try `vibe`)\n", v); return; }
                vibe = (HapticPatternId)id;
            }
            else if (strcasecmp(k, "led") == 0) {
                int id = _resolveLedId(v);
                if (id < 0) { Serial.printf("unknown led '%s'  (try `led`)\n", v); return; }
                led = (LedPatternId)id;
            }
            else if (strcasecmp(k, "rssi") == 0) rssi = (int8_t)atoi(v);
            else if (strcasecmp(k, "id")   == 0) ident = v;
            else Serial.printf("ignoring unknown key '%s'\n", k);
        }
        uint16_t aid = g_alerts.raise(title, type, vibe, led, ident, rssi);
        if (aid == AlertsService::INVALID_ALERT) Serial.println("ERR: alert capacity full (ack some first)");
        else                                     Serial.printf("OK: alert id=%u\n", (unsigned)aid);
        return;
    }

    if (sub && strcasecmp(sub, "ack") == 0) {
        if (!rest) { Serial.println("usage: alert ack <id>"); return; }
        uint16_t id = (uint16_t)atoi(rest);
        Serial.println(g_alerts.ack(id) ? "OK" : "no such alert id");
        return;
    }

    if (sub && strcasecmp(sub, "clear") == 0) {
        const uint8_t n_before = g_alerts.count();
        g_alerts.clearAll();
        Serial.printf("OK: cleared %u alert%s\n",
                      (unsigned)n_before, n_before == 1 ? "" : "s");
        return;
    }

    Serial.printf("unknown subcommand 'alert %s'  (try 'alert')\n", sub ? sub : "");
}

// ---------------------------------------------------------------------------
// `scan` command — manage the scan-result ring + seen-devices map. Until the
// real BLE/WiFi callbacks exist, `scan inject` is how we drive the rules
// engine for development.
//
//   scan                                  → list seen devices (dedup'd by MAC)
//   scan list                             → same (explicit)
//   scan clear                            → wipe the seen map
//   scan inject domain=bt mac=AA:.. rssi=-50 name=AirTag mfg=0x004C
//
// All inject fields are k=v. `name=` cannot contain spaces (strtok splits on
// whitespace); use underscores or omit if not needed. Real scan callbacks
// will populate the field directly when they land.
// ---------------------------------------------------------------------------

static const char* _scanDomainName(ScanDomain d) {
    return d == SCAN_WIFI ? "wifi" : "ble";
}

static bool _parseScanDomain(const char* s, ScanDomain* out) {
    if (!s || !out) return false;
    if (strcasecmp(s, "ble")  == 0 || strcasecmp(s, "bt") == 0) { *out = SCAN_BLE;  return true; }
    if (strcasecmp(s, "wifi") == 0)                             { *out = SCAN_WIFI; return true; }
    return false;
}

// Parses a MacAddrType label (mirrors macTypeName) for `scan inject type=`.
// Test-only convenience so the `scan list` type column can be exercised
// without a real advertiser. "random" maps to RPA arbitrarily — RPA and NRPA
// both display as "random" anyway; rpa/nrpa remain accepted for precision.
static bool _parseMacType(const char* s, uint8_t* out) {
    if (!s || !out) return false;
    if (strcasecmp(s, "static")   == 0) { *out = MAC_TYPE_PUBLIC;        return true; }
    if (strcasecmp(s, "rotating") == 0) { *out = MAC_TYPE_RANDOM_STATIC; return true; }
    if (strcasecmp(s, "random")   == 0) { *out = MAC_TYPE_RPA;           return true; }
    if (strcasecmp(s, "rpa")      == 0) { *out = MAC_TYPE_RPA;           return true; }
    if (strcasecmp(s, "nrpa")     == 0) { *out = MAC_TYPE_NRPA;          return true; }
    return false;
}

// Parses "AA:BB:CC:DD:EE:FF" (case-insensitive) into a 6-byte array.
// Returns false on any format error.
static bool _parseMac(const char* s, uint8_t out[6]) {
    if (!s) return false;
    unsigned int b[6];
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) return false;
        out[i] = (uint8_t)b[i];
    }
    return true;
}

void SerialConsole::_cmdScan(char* args) {
    if (!args) {
        Serial.println("scan: scan-result ring + seen-devices map");
        Serial.println("  scan list                     list seen devices (dedup'd by MAC)");
        Serial.println("  scan inject <k=v>             push a test scan result");
        // Example MAC deliberately avoids AA:BB:CC:DD:EE:FF — that exact
        // address matches the factory party.json rule and starts party mode.
        Serial.println("                                k=v: domain=bt|ble|wifi  mac=AA:BB:CC:11:22:33");
        Serial.println("                                     rssi=<int>  name=<string>  mfg=<0xNNNN>");
        Serial.println("                                     type=static|rotating|random");
        Serial.println("  scan clear                    wipe the seen-devices map");
        return;
    }

    if (strncasecmp(args, "list", 4) == 0) {
        const size_t n = g_scan_service.seenCount();
        Serial.printf("seen: %u device%s (ring writes: %lu)\n",
                      (unsigned)n, n == 1 ? "" : "s",
                      (unsigned long)g_scan_service.writePos());
        if (n == 0) return;
        Serial.println(" #  dom   mac                type      rssi  age      mfg   oui-org         mfg-org         name");
        Serial.println("--  ----  -----------------  --------  ----  -------  ----  --------------  --------------  ----");
        const uint32_t now = millis();
        char age_buf[16];
        for (size_t i = 0; i < n; i++) {
            const ScanResult& r = g_scan_service.seenAt(i);
            fmt_uptime(age_buf, sizeof(age_buf), now - r.timestamp_ms);
            char mfg_buf[8];
            if (r.mfg_id) snprintf(mfg_buf, sizeof(mfg_buf), "%04X", (unsigned)r.mfg_id);
            else          snprintf(mfg_buf, sizeof(mfg_buf), "----");
            // Show OUI-derived and mfg-id-derived vendor names side-by-side
            // (rather than one or the other) so the operator can spot a
            // disagreement between the MAC's owner and the mfg-data owner —
            // e.g. a chipset vendor MAC paired with a product vendor mfg id.
            const char* oui_org = vendor_for_mac(r.mac);
            const char* mfg_org = r.mfg_id ? vendor_mfg_lookup(r.mfg_id) : nullptr;
            if (!oui_org) oui_org = "----";
            if (!mfg_org) mfg_org = "----";
            // %.14s truncates each column to 14 chars to keep the table aligned;
            // for the full name use `vendor oui <prefix>` or `vendor mfg <id>`.
            Serial.printf("%2u  %-4s  %02X:%02X:%02X:%02X:%02X:%02X  %-8s  %4d  %-7s  %s  %-14.14s  %-14.14s  %s\n",
                          (unsigned)i,
                          _scanDomainName((ScanDomain)r.domain),
                          r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5],
                          macTypeName(r.mac_type),
                          (int)r.rssi,
                          age_buf,
                          mfg_buf,
                          oui_org,
                          mfg_org,
                          r.name);
        }
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "clear") == 0) {
        g_scan_service.clear();
        Serial.println("OK: seen-devices map cleared");
        return;
    }

    if (sub && strcasecmp(sub, "inject") == 0) {
        if (!rest) {
            Serial.println("usage: scan inject domain=bt|wifi mac=AA:BB:CC:11:22:33 "
                           "[rssi=-50] [name=foo] [mfg=0x004C] [type=random] "
                           "[frame=probe|beacon] [ie_sig=2;12;221:506f9a...]");
            return;
        }

        ScanResult r{};
        r.domain       = SCAN_BLE;
        r.rssi         = INT8_MIN;
        r.timestamp_ms = millis();
        bool have_mac  = false;

        for (char* tok = strtok(rest, " "); tok; tok = strtok(nullptr, " ")) {
            char* eq = strchr(tok, '=');
            if (!eq) { Serial.printf("ignoring '%s' (expected k=v)\n", tok); continue; }
            *eq = '\0';
            const char* k = tok;
            const char* v = eq + 1;
            if (strcasecmp(k, "domain") == 0) {
                ScanDomain d;
                if (!_parseScanDomain(v, &d)) {
                    Serial.printf("bad domain '%s' (expected bt|ble|wifi)\n", v);
                    return;
                }
                r.domain = d;
            } else if (strcasecmp(k, "mac") == 0) {
                if (!_parseMac(v, r.mac)) {
                    Serial.printf("bad mac '%s' (expected AA:BB:CC:DD:EE:FF)\n", v);
                    return;
                }
                have_mac = true;
            } else if (strcasecmp(k, "rssi") == 0) {
                r.rssi = (int8_t)atoi(v);
            } else if (strcasecmp(k, "name") == 0) {
                strncpy(r.name, v, sizeof(r.name) - 1);
                r.name[sizeof(r.name) - 1] = '\0';
            } else if (strcasecmp(k, "mfg") == 0) {
                r.mfg_id = (uint16_t)strtoul(v, nullptr, 0);   // base=0 → 0x prefix auto
            } else if (strcasecmp(k, "type") == 0) {
                if (!_parseMacType(v, &r.mac_type)) {
                    Serial.printf("bad type '%s' (expected static|rotating|random)\n", v);
                    return;
                }
            } else if (strcasecmp(k, "frame") == 0) {
                if      (strcasecmp(v, "probe")  == 0 ||
                         strcasecmp(v, "probe_req") == 0) r.frame_kind = FRAME_PROBE_REQ;
                else if (strcasecmp(v, "beacon") == 0)    r.frame_kind = FRAME_BEACON;
                else {
                    Serial.printf("bad frame '%s' (expected probe|beacon)\n", v);
                    return;
                }
            } else if (strcasecmp(k, "ie_sig") == 0) {
                strncpy(r.ie_sig, v, sizeof(r.ie_sig) - 1);
                r.ie_sig[sizeof(r.ie_sig) - 1] = '\0';
            } else {
                Serial.printf("ignoring unknown key '%s'\n", k);
            }
        }

        if (!have_mac) {
            Serial.println("ERR: mac= is required");
            return;
        }

        g_scan_service.publish(r);
        Serial.printf("OK: injected %s %02X:%02X:%02X:%02X:%02X:%02X (seen: %u)\n",
                      _scanDomainName((ScanDomain)r.domain),
                      r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5],
                      (unsigned)g_scan_service.seenCount());
        return;
    }

    Serial.printf("unknown subcommand 'scan %s'  (try 'scan')\n", sub ? sub : "");
}

// ---------------------------------------------------------------------------
// `vendor` — query the raw IEEE OUI + BT SIG company tables. No curation:
// names returned are the verbatim upstream org name.
//
//   vendor                          → table sizes + usage
//   vendor oui <AA:BB:CC>           → forward OUI lookup
//   vendor mfg <0xNNNN>             → forward mfg-id lookup
//   vendor search <substring>       → substring scan over both tables;
//                                     same path RulesService takes at
//                                     rule load time.
// ---------------------------------------------------------------------------

// Case-insensitive substring match. needle_lc must already be lowercased.
// Avoids depending on GNU strcasestr.
static const char* _icontains(const char* haystack, const char* needle_lc) {
    if (!haystack || !needle_lc || !*needle_lc) return haystack;
    const size_t nlen = strlen(needle_lc);
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        for (; i < nlen; i++) {
            char a = p[i];
            if (!a) return nullptr;
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != needle_lc[i]) break;
        }
        if (i == nlen) return p;
    }
    return nullptr;
}

void SerialConsole::_cmdVendor(char* args) {
    if (!args) {
        Serial.println("vendor: IEEE OUI + BT SIG company lookup tables");
        Serial.println("  vendor stats                  table sizes (# OUIs / # mfg ids)");
        Serial.println("  vendor oui <AA:BB:CC>         resolve a 24-bit prefix to an org name");
        Serial.println("  vendor mfg <0xNNNN>           resolve a BT SIG company id to an org name");
        Serial.println("  vendor search <substring>    list OUIs / mfg ids whose name contains substring");
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "stats") == 0) {
        Serial.printf("vendor tables: %u OUIs, %u mfg ids (IEEE + BT SIG)\n",
                      (unsigned)vendor_oui_count(),
                      (unsigned)vendor_mfg_count());
        return;
    }

    if (sub && strcasecmp(sub, "oui") == 0) {
        if (!rest) { Serial.println("usage: vendor oui <AA:BB:CC>"); return; }
        unsigned int b[6] = {0};
        const int n = sscanf(rest, "%2x:%2x:%2x:%2x:%2x:%2x",
                             &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
        if (n < 3) {
            Serial.printf("bad oui '%s' (expected AA:BB:CC)\n", rest);
            return;
        }
        const uint32_t prefix = ((uint32_t)b[0] << 16) |
                                ((uint32_t)b[1] <<  8) |
                                ((uint32_t)b[2]);
        const char* name = vendor_oui_lookup(prefix);
        Serial.printf("%02X:%02X:%02X -> %s\n",
                      b[0], b[1], b[2], name ? name : "(unknown)");
        return;
    }

    if (sub && strcasecmp(sub, "mfg") == 0) {
        if (!rest) { Serial.println("usage: vendor mfg <0xNNNN>"); return; }
        const uint16_t id   = (uint16_t)strtoul(rest, nullptr, 0);
        const char*    name = vendor_mfg_lookup(id);
        Serial.printf("0x%04X -> %s\n", (unsigned)id, name ? name : "(unknown)");
        return;
    }

    if (sub && strcasecmp(sub, "search") == 0) {
        if (!rest || !*rest) { Serial.println("usage: vendor search <substring>"); return; }

        // Lowercase the needle once.
        char needle[48];
        size_t i = 0;
        for (; rest[i] && i < sizeof(needle) - 1; i++) {
            char c = rest[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            needle[i] = c;
        }
        needle[i] = '\0';

        // Cap output so a popular substring like "inc" doesn't flood the
        // terminal. The TOTAL count is still reported.
        constexpr size_t LIMIT = 40;
        size_t oui_total = 0, oui_shown = 0;
        size_t mfg_total = 0, mfg_shown = 0;

        Serial.printf("search '%s':\n", needle);

        for (size_t k = 0; k < vendor_oui_count(); k++) {
            uint32_t   p;
            const char* nm;
            vendor_oui_at(k, &p, &nm);
            if (!_icontains(nm, needle)) continue;
            oui_total++;
            if (oui_shown < LIMIT) {
                Serial.printf("  OUI %02X:%02X:%02X  %s\n",
                              (unsigned)((p >> 16) & 0xFF),
                              (unsigned)((p >>  8) & 0xFF),
                              (unsigned)( p        & 0xFF),
                              nm);
                oui_shown++;
            }
        }
        for (size_t k = 0; k < vendor_mfg_count(); k++) {
            uint16_t   id;
            const char* nm;
            vendor_mfg_at(k, &id, &nm);
            if (!_icontains(nm, needle)) continue;
            mfg_total++;
            if (mfg_shown < LIMIT) {
                Serial.printf("  MFG 0x%04X     %s\n", (unsigned)id, nm);
                mfg_shown++;
            }
        }

        Serial.printf("matched %u OUI%s, %u mfg id%s",
                      (unsigned)oui_total, oui_total == 1 ? "" : "s",
                      (unsigned)mfg_total, mfg_total == 1 ? "" : "s");
        if (oui_total > LIMIT || mfg_total > LIMIT) {
            Serial.printf("  (showed first %u of each)", (unsigned)LIMIT);
        }
        Serial.println();
        return;
    }

    Serial.printf("unknown subcommand 'vendor %s'  (try 'vendor')\n", sub ? sub : "");
}

// ---------------------------------------------------------------------------
// `rule` / `rules` — rules engine surface. See rules_service.h for the model.
//
// Following the singular/plural convention used elsewhere in this console:
//   `rule  <verb>` — create/mutate one rule
//   `rules <verb>` — list/show/toggle/inspect existing rules
// ---------------------------------------------------------------------------

static const char* _alertTypeNameForRule(AlertType t) {
    if (t == ALERT_TYPE_COUNT) return "inferred";
    switch (t) {
        case ALERT_BLE:         return "ble";
        case ALERT_WIFI:        return "wifi";
        case ALERT_SYSTEM:      return "sys";
        case ALERT_BATTERY_LOW: return "batt";
        default:                return "?";
    }
}

static const char* _ruleActionName(RuleAction a) {
    switch (a) {
        case RULE_ACTION_ALERT: return "alert";
        case RULE_ACTION_PARTY: return "party";
        default:                return "?";
    }
}

// Pretty-print one criterion. Used by `rules show`.
static void _printCriterion(uint16_t idx, const Criterion& c) {
    switch (c.kind) {
        case CRIT_OUI: {
            const uint8_t full = c.v.oui.nibbles >> 1;
            char pfx[20];
            int p = 0;
            for (uint8_t i = 0; i < full; i++) {
                p += snprintf(pfx + p, sizeof(pfx) - p, "%s%02X", i ? ":" : "", c.v.oui.bytes[i]);
            }
            if (c.v.oui.nibbles & 1) {
                p += snprintf(pfx + p, sizeof(pfx) - p, "%s%X",
                              full ? ":" : "", (c.v.oui.bytes[full] >> 4) & 0xF);
            }
            pfx[p] = '\0';
            // Vendor lookup is 24-bit MA-L; for a longer prefix this names the
            // umbrella block owner (e.g. IEEE RA for an MA-S range).
            const uint32_t p24 = ((uint32_t)c.v.oui.bytes[0] << 16) |
                                 ((uint32_t)c.v.oui.bytes[1] <<  8) |
                                 ((uint32_t)c.v.oui.bytes[2]);
            const char* org = vendor_oui_lookup(p24);
            Serial.printf("    [%u] oui  %-17s  (%s)\n", idx, pfx, org ? org : "?");
            break;
        }
        case CRIT_MAC:
            Serial.printf("    [%u] mac  %02X:%02X:%02X:%02X:%02X:%02X\n",
                          idx,
                          c.v.mac[0], c.v.mac[1], c.v.mac[2],
                          c.v.mac[3], c.v.mac[4], c.v.mac[5]);
            break;
        case CRIT_MFG: {
            const char* org = vendor_mfg_lookup(c.v.mfg_id);
            Serial.printf("    [%u] mfg  0x%04X  (%s)\n",
                          idx, (unsigned)c.v.mfg_id, org ? org : "?");
            break;
        }
        case CRIT_SERVICE: {
            // Print as 128-bit UUID, big-endian display.
            Serial.printf("    [%u] service  ", idx);
            for (int i = 15; i >= 0; i--) {
                Serial.printf("%02X", c.v.uuid[i]);
                if (i == 12 || i == 10 || i == 8 || i == 6) Serial.print('-');
            }
            Serial.println();
            break;
        }
        case CRIT_NAME_MATCH:
            Serial.printf("    [%u] name ~ \"%s\"\n", idx, c.v.str ? c.v.str : "");
            break;
        case CRIT_SSID_MATCH:
            Serial.printf("    [%u] ssid ~ \"%s\"  (wifi only)\n", idx, c.v.str ? c.v.str : "");
            break;
        case CRIT_OUI_ORG:
            Serial.printf("    [%u] oui_org ~ \"%s\"\n", idx, c.v.str ? c.v.str : "");
            break;
        case CRIT_MFG_ORG:
            Serial.printf("    [%u] mfg_org ~ \"%s\"\n", idx, c.v.str ? c.v.str : "");
            break;
        default:
            Serial.printf("    [%u] (unknown kind %u)\n", idx, (unsigned)c.kind);
            break;
    }
}

// Full per-rule dump. Shared by `rules` (lists all) and `rules show <name>`.
static void _printRuleFull(const Rule& r) {
    Serial.printf("%s  [%s, %s]\n",
                  r.name,
                  r.enabled ? "enabled" : "disabled",
                  r.is_factory ? "factory" : "user");
    Serial.printf("  title:     %s\n", r.title);
    Serial.printf("  type:      %s\n", _alertTypeNameForRule(r.alert_type));
    Serial.printf("  vibe:      %s%s\n",
                  r.vibe == HAPTIC_PATTERN_COUNT ? "(default) " : "",
                  r.vibe == HAPTIC_PATTERN_COUNT ? vibePatternName(HAPTIC_DOUBLE_TAP)
                                                 : vibePatternName(r.vibe));
    Serial.printf("  led:       %s%s\n",
                  r.led == LED_PATTERN_COUNT ? "(default) " : "",
                  r.led == LED_PATTERN_COUNT ? ledPatternName(LED_PATTERN_ALERT_DEFAULT)
                                             : ledPatternName(r.led));
    Serial.printf("  action:    %s\n", _ruleActionName(r.action));
    Serial.printf("  matches:   %lu\n", (unsigned long)r.match_count);
    Serial.printf("  criteria (%u):\n", (unsigned)r.criterion_count);
    for (uint16_t i = 0; i < r.criterion_count; i++) {
        _printCriterion(i, r.criteria[i]);
    }
}

void SerialConsole::_cmdRule(char* args) {
    if (!args) {
        Serial.println("rule: rules engine — JSON-backed match rules");
        Serial.println("  rule list                     dump every rule with criteria");
        Serial.println("  rule show <name>              details for one rule");
        Serial.println("  rule create <name> [k=v]      new rule (user file)");
        Serial.println("                                k=v: title=...  vibe=<name>  led=<name>");
        Serial.println("                                     type=ble|wifi|sys|batt|auto  action=alert|party");
        Serial.println("  rule add <name> <f>=<v>...    add one or more criteria (space-separated clauses)");
        Serial.println("                                f: oui mac mfg service name ssid oui_org mfg_org");
        Serial.println("                                   name/ssid/oui_org/mfg_org take match patterns:");
        Serial.println("                                     x=equals  .*x.*=contains  x.*=starts  .*x=ends");
        Serial.println("                                     plus \\d \\w [a-z] etc. (see docs/WRITING_RULES.md)");
        Serial.println("                                v: comma-separated values (underscore = space in strings)");
        Serial.println("  rule rm <name> <idx>          remove the Nth criterion");
        Serial.println("  rule delete <name>            delete a user rule");
        Serial.println("  rule enable <name>            enable (NVS overlay)");
        Serial.println("  rule disable <name>           disable");
        Serial.println("  rule reload                   wipe in-memory + re-read /rules/{factory,user}");
        Serial.println("  rule stats                    matches / ring-drain counters");
        Serial.println("  rule dump                     all rules as one JSON line (web companion)");
        Serial.println("  rule save <json>              create/replace a user rule from JSON (web companion)");
        return;
    }

    char* sub  = strtok(args, " ");
    char* rest = strtok(nullptr, "");

    if (sub && strcasecmp(sub, "list") == 0) {
        const uint16_t n = g_rules.count();
        if (n == 0) {
            Serial.println("no rules loaded (try 'rule create <name>')");
            return;
        }
        for (uint16_t i = 0; i < n; i++) {
            const Rule* r = g_rules.get(i);
            if (!r) continue;
            if (i > 0) Serial.println();
            _printRuleFull(*r);
        }
        return;
    }

    if (sub && strcasecmp(sub, "show") == 0) {
        if (!rest) { Serial.println("usage: rule show <name>"); return; }
        const Rule* r = g_rules.find(rest);
        if (!r) { Serial.printf("no rule '%s'\n", rest); return; }
        _printRuleFull(*r);
        return;
    }

    if (sub && (strcasecmp(sub, "enable") == 0 || strcasecmp(sub, "disable") == 0)) {
        if (!rest) { Serial.printf("usage: rule %s <name>\n", sub); return; }
        const bool en = (strcasecmp(sub, "enable") == 0);
        Serial.println(g_rules.setEnabled(rest, en) ? "OK" : "no such rule");
        return;
    }

    // Machine-readable pair for the web companion.
    if (sub && strcasecmp(sub, "dump") == 0) {
        g_rules.dumpJson(Serial);
        return;
    }
    if (sub && strcasecmp(sub, "save") == 0) {
        if (!rest) { Serial.println("{\"ok\":false}"); return; }
        Serial.printf("{\"ok\":%s}\n", g_rules.saveRuleFromJson(rest) ? "true" : "false");
        return;
    }

    if (sub && strcasecmp(sub, "stats") == 0) {
        uint16_t total = g_rules.count();
        uint16_t enabled = 0;
        for (uint16_t i = 0; i < total; i++) {
            const Rule* r = g_rules.get(i);
            if (r && r->enabled) enabled++;
        }
        Serial.printf("rulesets: %u loaded (%u enabled, %u disabled)\n",
                      (unsigned)total, (unsigned)enabled, (unsigned)(total - enabled));
        Serial.printf("rules:    %lu total across all rulesets\n",
                      (unsigned long)g_rules.totalRules());
        Serial.printf("matches:  %lu total since boot\n",
                      (unsigned long)g_rules.totalMatches());
        Serial.printf("lost:     %lu scan results dropped (ring overrun)\n",
                      (unsigned long)g_rules.lostScans());
        Serial.printf("ring:     read=%lu  write=%lu  lag=%lu\n",
                      (unsigned long)g_rules.ringReadPos(),
                      (unsigned long)g_scan_service.writePos(),
                      (unsigned long)(g_scan_service.writePos() - g_rules.ringReadPos()));
        return;
    }

    if (sub && strcasecmp(sub, "reload") == 0) {
        const uint16_t n  = g_rules.reloadFromFs();
        const uint32_t nr = g_rules.totalRules();
        Serial.printf("OK: reloaded %u ruleset%s with %lu rule%s\n",
                      (unsigned)n, n == 1 ? "" : "s",
                      (unsigned long)nr, nr == 1 ? "" : "s");
        return;
    }

    if (sub && strcasecmp(sub, "create") == 0) {
        if (!rest) { Serial.println("usage: rule create <name> [title=... vibe=... led=... type=... action=...]"); return; }
        // First token of rest is the rule name; remaining tokens are k=v.
        char* name = strtok(rest, " ");
        char* kvs  = strtok(nullptr, "");
        if (!name) { Serial.println("usage: rule create <name> ..."); return; }
        if (!g_rules.createRule(name)) {
            Serial.printf("ERR: could not create '%s' (duplicate or capacity)\n", name);
            return;
        }
        // Apply optional k=v pairs.
        if (kvs) {
            for (char* tok = strtok(kvs, " "); tok; tok = strtok(nullptr, " ")) {
                char* eq = strchr(tok, '=');
                if (!eq) { Serial.printf("ignoring '%s' (expected k=v)\n", tok); continue; }
                *eq = '\0';
                const char* k = tok;
                char*       v = eq + 1;
                // Title can be multi-word — underscores stand in for spaces.
                // Other fields (vibe, led, type, action) use registry names
                // that legitimately contain underscores (`double_tap`,
                // `red_blue`), so leave those alone.
                if (strcasecmp(k, "title") == 0) {
                    for (char* p = v; *p; p++) if (*p == '_') *p = ' ';
                }
                if (!g_rules.setRuleField(name, k, v)) {
                    Serial.printf("WARN: bad field/value: %s=%s\n", k, v);
                }
            }
        }
        Serial.printf("OK: rule '%s' created\n", name);
        return;
    }

    if (sub && strcasecmp(sub, "add") == 0) {
        if (!rest) { Serial.println("usage: rule add <name> <field>=<v1>[,<v2>] [<field>=<v>...]"); return; }
        char* name = strtok(rest, " ");
        if (!name) { Serial.println("usage: rule add <name> <field>=<values>"); return; }

        // Each remaining whitespace-delimited token is one field=values clause.
        // Comma-separated values within a clause are handled by addCriteria.
        int total = 0;
        for (char* tok = strtok(nullptr, " "); tok; tok = strtok(nullptr, " ")) {
            char* eq = strchr(tok, '=');
            if (!eq) {
                Serial.printf("ignoring '%s' (expected field=values)\n", tok);
                continue;
            }
            *eq = '\0';
            const char* field  = tok;
            const char* values = eq + 1;
            int added = g_rules.addCriteria(name, field, values);
            if (added < 0) {
                Serial.printf("ERR: bad field '%s' or unparseable value\n", field);
                continue;
            }
            total += added;
        }
        Serial.printf("OK: added %d criteri%s\n", total, total == 1 ? "on" : "a");
        return;
    }

    if (sub && strcasecmp(sub, "rm") == 0) {
        if (!rest) { Serial.println("usage: rule rm <name> <idx>"); return; }
        char* name = strtok(rest, " ");
        char* idxs = strtok(nullptr, " ");
        if (!name || !idxs) { Serial.println("usage: rule rm <name> <idx>"); return; }
        uint16_t idx = (uint16_t)atoi(idxs);
        Serial.println(g_rules.removeCriterion(name, idx) ? "OK" : "ERR: no such rule or idx");
        return;
    }

    if (sub && strcasecmp(sub, "delete") == 0) {
        if (!rest) { Serial.println("usage: rule delete <name>"); return; }
        Serial.println(g_rules.deleteRule(rest) ? "OK" : "no such rule");
        return;
    }

    Serial.printf("unknown subcommand 'rule %s'  (try 'rule')\n", sub ? sub : "");
}

// `power <subcmd>` — sleep / wake / shipping / reboot operations.
void SerialConsole::_cmdPower(char* args) {
    if (!args) {
        Serial.println("power: device power operations");
        Serial.println("  power sleep                   deep-sleep until long-press CENTER or wake timer");
        Serial.println("  power sleepscreen             turn screen off without deep sleep");
        Serial.println("  power reboot                  restart the device");
        Serial.println("  power shipping                factory wipe (settings/rules/alerts) + shipping sleep — long-press CENTER to wake");
        Serial.println("  power wipe                    factory wipe + reboot — comes back up like a first boot");
        Serial.println("  power off                     power down (no timer) — long-press CENTER to wake, keeps tutorial state");
        return;
    }

    char* sub = strtok(args, " ");

    // Every subcommand routes through the Power Action screen (same visual
    // beat as the power menu buttons): message on screen, short hold, then
    // the CMD_POWER_* fires. beginAction wakes the display first, so this
    // works with the screen off too. The hold also replaces the old
    // delay(100)-to-flush-serial — replies go out long before teardown.
    struct { const char* name; EventId cmd; } const kActions[] = {
        { "sleep",       CMD_POWER_SLEEP          },
        { "sleepscreen", CMD_POWER_SCREEN_OFF     },
        { "reboot",      CMD_POWER_REBOOT         },
        { "shipping",    CMD_POWER_SHIPPING_RESET },
        { "wipe",        CMD_POWER_FACTORY_RESET  },
        { "off",         CMD_POWER_DOWN           },
    };
    for (const auto& a : kActions) {
        if (sub && strcasecmp(sub, a.name) == 0) {
            g_power_menu_screen.beginAction(a.cmd);
            Serial.println("OK");
            return;
        }
    }

    Serial.printf("unknown subcommand 'power %s'  (try 'power')\n", sub ? sub : "");
}
