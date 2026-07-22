#include "rules_service.h"
#include "scan_service.h"
#include "vendor_lookup.h"
#include "party_service.h"
#include "re_lite.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

RulesService g_rules;

// Disk paths
static constexpr const char* DIR_RULES         = "/rules";
static constexpr const char* DIR_FACTORY       = "/rules/factory";
static constexpr const char* DIR_USER          = "/rules/user";

// NVS namespace + key for the disabled-rules overlay. Single blob of
// newline-separated rule names; keeps key count fixed at 1 regardless of
// how many rules are toggled.
static constexpr const char* NVS_NAMESPACE     = "rules";
static constexpr const char* NVS_KEY_DISABLED  = "disabled";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Allocate string in PSRAM and copy. nullptr on OOM (shouldn't happen with
// 7+MB free at runtime; caller still checks).
static char* _psram_strdup(const char* s) {
    const size_t n = strlen(s) + 1;
    char* out = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out) memcpy(out, s, n);
    return out;
}

// Case-insensitive substring test with an explicit needle length (the needle
// is a slice of a longer pattern string, so it isn't NUL-terminated where we
// want to stop). Empty needle matches anything.
static bool _icontains_n(const char* haystack, const char* needle, size_t nlen) {
    if (nlen == 0) return true;
    if (!haystack) return false;
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        for (; i < nlen; i++) {
            char a = p[i];
            if (!a) return false;
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (i == nlen) return true;
    }
    return false;
}

// Classify a pattern into a match shape. For every shape but PAT_REGEX the
// literal "core" is [*off_out, *off_out + *len_out) within `p` — i.e. `p` with
// its leading/trailing ".*" wildcards skipped. PAT_REGEX ignores off/len and
// runs the whole verbatim pattern through re_lite. Assumes `p` already passed
// re_lite_valid(). See PatShape in rules_service.h.
static PatShape _classifyPattern(const char* p, uint8_t* off_out, uint8_t* len_out) {
    const size_t n = strlen(p);
    bool lead  = (n >= 2 && p[0] == '.' && p[1] == '*');
    bool trail = (n >= 2 && p[n - 1] == '*' && p[n - 2] == '.');
    // A "\.*" tail is an escaped literal dot, not a wildcard — don't strip it.
    if (trail && n >= 3 && p[n - 3] == '\\') trail = false;

    size_t start = lead  ? 2      : 0;
    size_t end   = trail ? n - 2  : n;
    if (end < start) end = start;

    for (size_t i = start; i < end; i++) {
        const char c = p[i];
        if (c == '.' || c == '^' || c == '$' || c == '*' || c == '+' ||
            c == '?' || c == '[' || c == ']' || c == '(' || c == ')' ||
            c == '{' || c == '}' || c == '|' || c == '\\') {
            return PAT_REGEX;   // a metachar survived affix-stripping
        }
    }
    *off_out = (uint8_t)start;
    *len_out = (uint8_t)(end - start);
    if (lead && trail) return PAT_CONTAINS;   // .*core.*
    if (lead)          return PAT_SUFFIX;     // .*core
    if (trail)         return PAT_PREFIX;     // core.*
    return PAT_EXACT;                         // core
}

// Evaluate a classified pattern against a haystack. Fast-path shapes are plain
// case-insensitive compares; only PAT_REGEX invokes the matcher.
static bool _patMatch(const char* pat, PatShape shape, uint8_t off, uint8_t len,
                      const char* hay) {
    if (!hay) hay = "";
    switch (shape) {
        case PAT_EXACT:    return strcasecmp(hay, pat) == 0;
        case PAT_CONTAINS: return _icontains_n(hay, pat + off, len);
        case PAT_PREFIX:   return strncasecmp(hay, pat + off, len) == 0;
        case PAT_SUFFIX: {
            const size_t hl = strlen(hay);
            if (hl < len) return false;
            return strncasecmp(hay + hl - len, pat + off, len) == 0;
        }
        case PAT_REGEX:    return re_lite_full_match(pat, hay);
    }
    return false;
}

// Parse an OUI/MAC prefix into `bytes` (MSB-first) + a nibble count. Accepts
// 6..12 hex nibbles (24-bit MA-L .. 48-bit full MAC) with optional ':' / '-'
// separators, so "00:1D:96" (24-bit), "8C:1F:64:F" (28-bit MA-M) and
// "70:B3:D5:1A:2" (36-bit MA-S) all parse. An odd nibble lands in the high half
// of the last byte, low half zeroed. Returns false on a non-hex char, or a
// length below 3 octets / above a full MAC.
static bool _parseOuiPrefix(const char* s, uint8_t bytes[6], uint8_t* nibbles_out) {
    if (!s) return false;
    memset(bytes, 0, 6);
    uint8_t nib = 0;
    for (const char* p = s; *p; p++) {
        if (*p == ':' || *p == '-') continue;      // separators optional
        int v;
        if      (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return false;                          // non-hex
        if (nib >= 12) return false;                // longer than a full MAC
        if ((nib & 1) == 0) bytes[nib >> 1] = (uint8_t)(v << 4);   // high nibble
        else                bytes[nib >> 1] |= (uint8_t)v;         // low nibble
        nib++;
    }
    if (nib < 6) return false;                      // shorter than 3 octets
    *nibbles_out = nib;
    return true;
}

static bool _parseMac6(const char* s, uint8_t out[6]) {
    if (!s) return false;
    unsigned int b[6] = {0};
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

static bool _parseMfgId(const char* s, uint16_t* out) {
    if (!s || !out) return false;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 0);   // 0x prefix auto-detected
    if (end == s || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

// Parses a service UUID — accepts short forms ("180F", "0x180F") promoted
// via the BLE base UUID, or a full 128-bit form ("0000180F-0000-1000-8000-
// 00805F9B34FB"). Output is stored in 16-byte little-endian wire order so
// it byte-compares to scan_uuids directly.
static bool _parseServiceUuid(const char* s, uint8_t out[16]) {
    if (!s) return false;

    // Short form: <=4 hex chars, optionally 0x-prefixed.
    if (strchr(s, '-') == nullptr) {
        char* end = nullptr;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s || v > 0xFFFFFFFF) return false;
        // BLE base UUID: 00000000-0000-1000-8000-00805F9B34FB
        // Bytes 12..15 (little-endian) hold the short value (big-endian).
        static const uint8_t base[16] = {
            0xFB, 0x9B, 0x34, 0x5F, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        memcpy(out, base, 16);
        out[12] = (uint8_t)( v        & 0xFF);
        out[13] = (uint8_t)((v >>  8) & 0xFF);
        out[14] = (uint8_t)((v >> 16) & 0xFF);
        out[15] = (uint8_t)((v >> 24) & 0xFF);
        return true;
    }

    // Full UUID form. Read big-endian then byte-reverse to wire order.
    unsigned int u[16] = {0};
    int n = sscanf(s,
        "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &u[15], &u[14], &u[13], &u[12],
        &u[11], &u[10], &u[ 9], &u[ 8],
        &u[ 7], &u[ 6], &u[ 5], &u[ 4],
        &u[ 3], &u[ 2], &u[ 1], &u[ 0]);
    if (n != 16) return false;
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)u[i];
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RulesService::begin(EventBus& bus) {
    _bus = &bus;
    // Start draining the ring from "right now" — old scan entries from before
    // we existed aren't actionable. ScanService.publish() since boot will be
    // visible on the next tick.
    _ring_read_pos = g_scan_service.writePos();

    // LittleFS must already be mounted by main.cpp.
    _ensureUserDir();
    _loading = true;   // don't let loading a rule persist it back to disk
    _loadDir(DIR_FACTORY, true);
    _loadDir(DIR_USER,    false);
    _loading = false;
    _applyEnabledOverlay();

    const uint32_t nrules = totalRules();
    Serial.printf("[rules] loaded %u ruleset%s with %lu rule%s\n",
                  (unsigned)_count, _count == 1 ? "" : "s",
                  (unsigned long)nrules, nrules == 1 ? "" : "s");
}

uint16_t RulesService::reloadFromFs() {
    while (_count > 0) {
        _freeRuleContents(_rules[_count - 1]);
        memset(&_rules[_count - 1], 0, sizeof(Rule));
        _count--;
    }
    _ensureUserDir();
    _loading = true;
    _loadDir(DIR_FACTORY, true);
    _loadDir(DIR_USER,    false);
    _loading = false;
    _applyEnabledOverlay();
    _notifyChanged();
    return _count;
}

void RulesService::tick() {
    ScanResult batch[DRAIN_BATCH];
    uint32_t   lost = 0;
    const size_t got = g_scan_service.drain(&_ring_read_pos, batch, DRAIN_BATCH, &lost);
    _lost_scans += lost;
    for (size_t i = 0; i < got; i++) _matchScan(batch[i]);
}

void RulesService::onEvent(const Event& /*e*/) {
    // RulesService doesn't currently subscribe to anything — drain runs from
    // tick(). Hook reserved for future CMD_RULE_* events.
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

void RulesService::_notifyChanged() {
    // No payload — consumers re-read g_rules. Suppressed during bulk FS loads
    // (begin/reloadFromFs post once when the load completes, not per file).
    if (_bus && !_loading) _bus->post(EV_RULES_CHANGED);
}

int RulesService::_findRuleIdx(const char* name) const {
    if (!name) return -1;
    for (uint16_t i = 0; i < _count; i++) {
        if (strcasecmp(_rules[i].name, name) == 0) return (int)i;
    }
    return -1;
}

const Rule* RulesService::find(const char* name) const {
    int i = _findRuleIdx(name);
    return i >= 0 ? &_rules[i] : nullptr;
}

const Rule* RulesService::get(uint16_t idx) const {
    if (idx >= _count) return nullptr;
    return &_rules[idx];
}

bool RulesService::_ensureCapacity(Rule& r, uint16_t need) {
    if (r.criterion_cap >= need) return true;
    uint16_t new_cap = r.criterion_cap ? r.criterion_cap : 4;
    while (new_cap < need) new_cap = (uint16_t)(new_cap * 2);
    if (new_cap > MAX_CRITERIA) new_cap = MAX_CRITERIA;
    if (new_cap < need) return false;
    Criterion* p = (Criterion*)heap_caps_realloc(
        r.criteria, sizeof(Criterion) * new_cap,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return false;
    r.criteria     = p;
    r.criterion_cap = new_cap;
    return true;
}

bool RulesService::_appendCriterion(Rule& r, const Criterion& c) {
    if (!_ensureCapacity(r, (uint16_t)(r.criterion_count + 1))) return false;
    r.criteria[r.criterion_count++] = c;
    return true;
}

void RulesService::_freeCriterion(Criterion& c) {
    switch (c.kind) {
        case CRIT_NAME_MATCH:
        case CRIT_SSID_MATCH:
        case CRIT_OUI_ORG:
        case CRIT_MFG_ORG:
            if (c.v.str) heap_caps_free((void*)c.v.str);
            c.v.str = nullptr;
            break;
        default:
            break;
    }
}

void RulesService::_freeRuleContents(Rule& r) {
    if (r.criteria) {
        for (uint16_t i = 0; i < r.criterion_count; i++) _freeCriterion(r.criteria[i]);
        heap_caps_free(r.criteria);
    }
    r.criteria         = nullptr;
    r.criterion_count  = 0;
    r.criterion_cap    = 0;
}

bool RulesService::createRule(const char* name) {
    if (!name || !*name) return false;
    if (_findRuleIdx(name) >= 0) return false;
    if (_count >= MAX_RULES) return false;

    Rule& r = _rules[_count];
    memset(&r, 0, sizeof(Rule));
    strncpy(r.name, name, sizeof(r.name) - 1);
    strncpy(r.title, name, sizeof(r.title) - 1);
    r.vibe        = HAPTIC_PATTERN_COUNT;
    r.led         = LED_PATTERN_COUNT;
    r.alert_type  = ALERT_TYPE_COUNT;     // infer from scan domain
    r.action      = RULE_ACTION_ALERT;
    r.is_factory  = false;
    r.enabled     = true;
    _count++;
    _saveUserRule(r);   // persist immediately so create survives reboot even before criteria added
    _notifyChanged();
    return true;
}

bool RulesService::deleteRule(const char* name) {
    int idx = _findRuleIdx(name);
    if (idx < 0) return false;
    if (_rules[idx].is_factory) return false;   // factory rules are read-only
    char saved_name[sizeof(Rule::name)];
    strncpy(saved_name, _rules[idx].name, sizeof(saved_name));
    saved_name[sizeof(saved_name) - 1] = '\0';
    _freeRuleContents(_rules[idx]);
    for (uint16_t i = (uint16_t)idx; i + 1 < _count; i++) {
        _rules[i] = _rules[i + 1];
    }
    memset(&_rules[_count - 1], 0, sizeof(Rule));
    _count--;
    _deleteUserRuleFile(saved_name);
    _notifyChanged();
    return true;
}

bool RulesService::setRuleField(const char* name, const char* field, const char* value) {
    int idx = _findRuleIdx(name);
    if (idx < 0 || !field || !value) return false;
    Rule& r = _rules[idx];
    if (r.is_factory) return false;   // factory rule content is read-only

    if (strcasecmp(field, "title") == 0) {
        strncpy(r.title, value, sizeof(r.title) - 1);
        r.title[sizeof(r.title) - 1] = '\0';
        _saveUserRule(r);
        _notifyChanged();
        return true;
    }
    if (strcasecmp(field, "vibe") == 0) {
        HapticPatternId p = vibePatternByName(value);
        if (p == HAPTIC_PATTERN_COUNT) return false;
        r.vibe = p;
        _saveUserRule(r);
        _notifyChanged();
        return true;
    }
    if (strcasecmp(field, "led") == 0) {
        LedPatternId p = ledPatternByName(value);
        if (p == LED_PATTERN_COUNT) return false;
        r.led = p;
        _saveUserRule(r);
        _notifyChanged();
        return true;
    }
    if (strcasecmp(field, "type") == 0 || strcasecmp(field, "alert_type") == 0) {
        if (strcasecmp(value, "ble")    == 0 || strcasecmp(value, "bt") == 0) r.alert_type = ALERT_BLE;
        else if (strcasecmp(value, "wifi") == 0)                              r.alert_type = ALERT_WIFI;
        else if (strcasecmp(value, "sys")  == 0 ||
                 strcasecmp(value, "system") == 0)                            r.alert_type = ALERT_SYSTEM;
        else if (strcasecmp(value, "batt") == 0 ||
                 strcasecmp(value, "battery") == 0 ||
                 strcasecmp(value, "low") == 0)                               r.alert_type = ALERT_BATTERY_LOW;
        else if (strcasecmp(value, "auto") == 0 ||
                 strcasecmp(value, "infer") == 0)                             r.alert_type = ALERT_TYPE_COUNT;
        else return false;
        _saveUserRule(r);
        _notifyChanged();
        return true;
    }
    if (strcasecmp(field, "action") == 0) {
        if      (strcasecmp(value, "alert") == 0) r.action = RULE_ACTION_ALERT;
        else if (strcasecmp(value, "party") == 0) r.action = RULE_ACTION_PARTY;
        else return false;
        _saveUserRule(r);
        _notifyChanged();
        return true;
    }
    return false;
}

bool RulesService::setEnabled(const char* name, bool enabled) {
    int idx = _findRuleIdx(name);
    if (idx < 0) return false;
    _rules[idx].enabled = enabled;
    _persistEnabledOverlay();   // NVS overlay survives reboots + FS reflash
    _notifyChanged();
    return true;
}

bool RulesService::removeCriterion(const char* name, uint16_t idx) {
    int rIdx = _findRuleIdx(name);
    if (rIdx < 0) return false;
    Rule& r = _rules[rIdx];
    if (r.is_factory) return false;
    if (idx >= r.criterion_count) return false;
    _freeCriterion(r.criteria[idx]);
    for (uint16_t i = idx; i + 1 < r.criterion_count; i++) {
        r.criteria[i] = r.criteria[i + 1];
    }
    r.criterion_count--;
    _saveUserRule(r);
    _notifyChanged();
    return true;
}

// ---------------------------------------------------------------------------
// Criterion-adding: parse one field=values pair into one or more Criterion
// entries.
// ---------------------------------------------------------------------------

int RulesService::addCriteria(const char* name, const char* field, const char* values_csv) {
    int rIdx = _findRuleIdx(name);
    if (rIdx < 0 || !field || !values_csv) return -1;
    Rule& r = _rules[rIdx];
    if (r.is_factory) return -1;
    const int added = _addCriteriaToRule(r, field, values_csv);
    if (added > 0) {
        _saveUserRule(r);
        _notifyChanged();
    }
    return added;
}

// Parse `field`=`values_csv` into criteria on `r`. No factory guard and no
// persistence (the public addCriteria wraps this with both; the loader calls it
// directly). Returns the count added, or -1 on parse error / invalid pattern.
int RulesService::_addCriteriaToRule(Rule& r, const char* field, const char* values_csv) {
    if (!field || !values_csv) return -1;

    // Identify the field. name/ssid/oui_org/mfg_org take case-insensitive
    // full-match patterns (see PatShape); the *_equals / *_contains fields they
    // replaced are gone.
    enum FieldKind {
        F_OUI, F_MAC, F_MFG, F_SERVICE,
        F_NAME, F_SSID, F_OUI_ORG, F_MFG_ORG,
        F_UNKNOWN
    };
    FieldKind fk =
        (strcasecmp(field, "oui")     == 0) ? F_OUI      :
        (strcasecmp(field, "mac")     == 0) ? F_MAC      :
        (strcasecmp(field, "mfg")     == 0) ? F_MFG      :
        (strcasecmp(field, "service") == 0) ? F_SERVICE  :
        (strcasecmp(field, "name")    == 0) ? F_NAME     :
        (strcasecmp(field, "ssid")    == 0) ? F_SSID     :
        (strcasecmp(field, "oui_org") == 0) ? F_OUI_ORG  :
        (strcasecmp(field, "mfg_org") == 0) ? F_MFG_ORG  :
        F_UNKNOWN;
    if (fk == F_UNKNOWN) return -1;

    // Tokenize values by comma. Mutate the input copy.
    char* buf = strdup(values_csv);
    if (!buf) return -1;

    int added = 0;
    char* save = nullptr;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        // Trim leading/trailing whitespace.
        while (*tok == ' ' || *tok == '\t') tok++;
        char* end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (!*tok) continue;

        // Underscores stand in for spaces in stringy values.
        for (char* p = tok; *p; p++) if (*p == '_') *p = ' ';

        Criterion c{};
        switch (fk) {
            case F_OUI: {
                uint8_t bytes[6]; uint8_t nib;
                if (!_parseOuiPrefix(tok, bytes, &nib)) { free(buf); return -1; }
                c.kind = CRIT_OUI;
                memcpy(c.v.oui.bytes, bytes, 6);
                c.v.oui.nibbles = nib;
                if (_appendCriterion(r, c)) added++;
                break;
            }
            case F_MAC: {
                uint8_t m[6];
                if (!_parseMac6(tok, m)) { free(buf); return -1; }
                c.kind = CRIT_MAC;
                memcpy(c.v.mac, m, 6);
                if (_appendCriterion(r, c)) added++;
                break;
            }
            case F_MFG: {
                uint16_t id;
                if (!_parseMfgId(tok, &id)) { free(buf); return -1; }
                c.kind = CRIT_MFG;
                c.v.mfg_id = id;
                if (_appendCriterion(r, c)) added++;
                break;
            }
            case F_SERVICE: {
                uint8_t u[16];
                if (!_parseServiceUuid(tok, u)) { free(buf); return -1; }
                c.kind = CRIT_SERVICE;
                memcpy(c.v.uuid, u, 16);
                if (_appendCriterion(r, c)) added++;
                break;
            }
            case F_NAME:
            case F_SSID:
            case F_OUI_ORG:
            case F_MFG_ORG: {
                // Pattern kinds. Store the pattern verbatim (original case) for
                // `rules show` and JSON round-trip, and classify its shape once
                // so the hot path skips the regex engine for plain shapes.
                // name/ssid match the device name; oui_org/mfg_org match the
                // vendor name resolved per sighting at match time.
                if (!re_lite_valid(tok)) { free(buf); return -1; }
                uint8_t off = 0, len = 0;
                const PatShape shape = _classifyPattern(tok, &off, &len);
                char* dup = _psram_strdup(tok);
                if (!dup) { free(buf); return -1; }
                c.kind      = (fk == F_NAME)    ? CRIT_NAME_MATCH :
                              (fk == F_SSID)    ? CRIT_SSID_MATCH :
                              (fk == F_OUI_ORG) ? CRIT_OUI_ORG    : CRIT_MFG_ORG;
                c.pat_shape = shape;
                c.pat_off   = off;
                c.pat_len   = len;
                c.v.str     = dup;
                if (_appendCriterion(r, c)) added++;
                else                        heap_caps_free(dup);
                break;
            }
            default:
                break;
        }
    }

    free(buf);
    return added;   // caller (public addCriteria) persists; the loader does not
}

// ---------------------------------------------------------------------------
// Match path
// ---------------------------------------------------------------------------

bool RulesService::_criterionMatches(const Criterion& c, const ScanResult& s,
                                     const char* oui_org, const char* mfg_org) const {
    switch (c.kind) {
        case CRIT_OUI: {
            // Nibble-wise prefix compare: full bytes, then a trailing high
            // nibble when the prefix has an odd nibble count.
            const uint8_t full = c.v.oui.nibbles >> 1;
            for (uint8_t i = 0; i < full; i++) {
                if (s.mac[i] != c.v.oui.bytes[i]) return false;
            }
            if (c.v.oui.nibbles & 1) {
                if ((s.mac[full] & 0xF0) != (c.v.oui.bytes[full] & 0xF0)) return false;
            }
            return true;
        }
        case CRIT_MAC:
            return memcmp(s.mac, c.v.mac, 6) == 0;
        case CRIT_MFG:
            return s.mfg_id != 0 && s.mfg_id == c.v.mfg_id;
        case CRIT_SERVICE:
            for (uint8_t i = 0; i < s.service_count && i < 4; i++) {
                if (memcmp(s.service_uuids[i], c.v.uuid, 16) == 0) return true;
            }
            return false;
        case CRIT_NAME_MATCH:
            return c.v.str && _patMatch(c.v.str, c.pat_shape, c.pat_off, c.pat_len, s.name);
        case CRIT_SSID_MATCH:
            return s.domain == SCAN_WIFI && c.v.str &&
                   _patMatch(c.v.str, c.pat_shape, c.pat_off, c.pat_len, s.name);
        case CRIT_OUI_ORG:
            return oui_org && c.v.str &&
                   _patMatch(c.v.str, c.pat_shape, c.pat_off, c.pat_len, oui_org);
        case CRIT_MFG_ORG:
            return mfg_org && c.v.str &&
                   _patMatch(c.v.str, c.pat_shape, c.pat_off, c.pat_len, mfg_org);
        default:
            return false;
    }
}

void RulesService::_matchScan(const ScanResult& s) {
    // Resolve the sighting's vendor names once and reuse them across every rule
    // — CRIT_OUI_ORG / CRIT_MFG_ORG match against these instead of expanding the
    // vendor table at load. Both are single bsearches; oui uses the 24-bit OUI.
    const char* oui_org = vendor_for_mac(s.mac);
    const char* mfg_org = s.mfg_id ? vendor_mfg_lookup(s.mfg_id) : nullptr;
    for (uint16_t i = 0; i < _count; i++) {
        Rule& r = _rules[i];
        if (!r.enabled || r.criterion_count == 0) continue;
        for (uint16_t k = 0; k < r.criterion_count; k++) {
            if (_criterionMatches(r.criteria[k], s, oui_org, mfg_org)) {
                _fire(r, s);
                _match_count++;
                r.match_count++;
                break;   // one match per (rule, scan) is enough
            }
        }
    }
}

void RulesService::_fire(Rule& r, const ScanResult& s) {
    if (r.action == RULE_ACTION_PARTY) {
        // Party mode owns its own effects (rainbow LEDs, haptics, dance anim,
        // banner) — no alert card. Re-fires while the device lingers just
        // refresh the party timer; from_rule=true honours the post-dismiss
        // cooldown so a persistent beacon can't instantly re-trigger.
        g_party.start(PartyService::DEFAULT_DURATION_MS, /*from_rule=*/true);
        return;
    }
    if (r.action != RULE_ACTION_ALERT) {
        return;   // other actions reserved
    }

    // Dedup identifier: per (rule, MAC). AlertsService coalesces re-fires
    // into a last_seen update. Sized for name[56] + ':' + 12 hex + NUL —
    // truncating the MAC off the key would collapse distinct devices into
    // one alert.
    char ident[72];
    static_assert(sizeof(ident) >= sizeof(Rule::name) + 13,
                  "ident must fit name + ':' + 12 hex MAC");
    snprintf(ident, sizeof(ident), "%s:%02X%02X%02X%02X%02X%02X",
             r.name,
             s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);

    AlertType type = r.alert_type;
    if (type == ALERT_TYPE_COUNT) {
        type = (s.domain == SCAN_BLE) ? ALERT_BLE : ALERT_WIFI;
    }
    HapticPatternId vibe = (r.vibe == HAPTIC_PATTERN_COUNT) ? HAPTIC_DOUBLE_TAP        : r.vibe;
    LedPatternId    led  = (r.led  == LED_PATTERN_COUNT)    ? LED_PATTERN_ALERT_DEFAULT : r.led;

    const uint16_t id = g_alerts.raise(r.title, type, vibe, led, ident, s.rssi);
    if (id == AlertsService::INVALID_ALERT) {
        // Store full (16 active) and this device has no existing record —
        // the alert is lost. Throttled: a present device re-fires several
        // times a second.
        static uint32_t s_last_warn_ms = 0;
        const uint32_t now = millis();
        if (now - s_last_warn_ms >= 5000) {
            s_last_warn_ms = now;
            Serial.printf("[rules] '%s' fired but alert store is full — ack or clear alerts\n",
                          r.name);
        }
    }
}

// ---------------------------------------------------------------------------
// LittleFS persistence
// ---------------------------------------------------------------------------

void RulesService::_ensureUserDir() {
    if (!LittleFS.exists(DIR_RULES))   LittleFS.mkdir(DIR_RULES);
    if (!LittleFS.exists(DIR_FACTORY)) LittleFS.mkdir(DIR_FACTORY);
    if (!LittleFS.exists(DIR_USER))    LittleFS.mkdir(DIR_USER);
}

void RulesService::_loadDir(const char* dir_path, bool is_factory) {
    File dir = LittleFS.open(dir_path);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("[rules] %s missing — skipping\n", dir_path);
        return;
    }
    File entry = dir.openNextFile();
    while (entry) {
        const char* fname = entry.name();
        // Skip non-JSON files and dotfiles. fname is the leaf — File::path()
        // returns the full LittleFS path which we need for open().
        if (!entry.isDirectory()
            && fname[0] != '.'
            && strstr(fname, ".json") != nullptr) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s", dir_path, fname);
            _loadRuleFromFile(path, is_factory);
        }
        entry = dir.openNextFile();
    }
}

// Translate an internal CriterionKind back into the on-disk field name.
// Used by _saveUserRule to group atomic criteria back into JSON entries.
static const char* _kindToField(CriterionKind k) {
    switch (k) {
        case CRIT_OUI:         return "oui";
        case CRIT_MAC:         return "mac";
        case CRIT_MFG:         return "mfg";
        case CRIT_SERVICE:     return "service";
        case CRIT_NAME_MATCH:  return "name";
        case CRIT_SSID_MATCH:  return "ssid";
        case CRIT_OUI_ORG:     return "oui_org";
        case CRIT_MFG_ORG:     return "mfg_org";
        default:               return nullptr;
    }
}

// Append `c`'s value to `arr` in whatever native form ArduinoJson handles
// for the kind. Strings are added directly; numerics formatted to a local
// buffer first so ArduinoJson copies them into its zone.
static void _appendCriterionValue(JsonArray arr, const Criterion& c) {
    char buf[40];
    switch (c.kind) {
        case CRIT_OUI: {
            // Emit the full bytes colon-joined; append a lone trailing nibble
            // for an odd count. 24-bit prefixes round-trip as "AA:BB:CC".
            const uint8_t full = c.v.oui.nibbles >> 1;
            int p = 0;
            for (uint8_t i = 0; i < full; i++) {
                p += snprintf(buf + p, sizeof(buf) - p, "%s%02X", i ? ":" : "", c.v.oui.bytes[i]);
            }
            if (c.v.oui.nibbles & 1) {
                p += snprintf(buf + p, sizeof(buf) - p, "%s%X",
                              full ? ":" : "", (c.v.oui.bytes[full] >> 4) & 0xF);
            }
            buf[p] = '\0';
            arr.add(buf);
            break;
        }
        case CRIT_MAC:
            snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     c.v.mac[0], c.v.mac[1], c.v.mac[2],
                     c.v.mac[3], c.v.mac[4], c.v.mac[5]);
            arr.add(buf);
            break;
        case CRIT_MFG:
            snprintf(buf, sizeof(buf), "0x%04X", (unsigned)c.v.mfg_id);
            arr.add(buf);
            break;
        case CRIT_SERVICE: {
            // Reverse the little-endian wire order back to big-endian display.
            int n = 0;
            for (int i = 15; i >= 0 && n < (int)sizeof(buf) - 1; i--) {
                n += snprintf(buf + n, sizeof(buf) - n, "%02X", c.v.uuid[i]);
                if (i == 12 || i == 10 || i == 8 || i == 6) {
                    if (n < (int)sizeof(buf) - 1) buf[n++] = '-';
                }
            }
            buf[n] = '\0';
            arr.add(buf);
            break;
        }
        case CRIT_NAME_MATCH:
        case CRIT_SSID_MATCH:
        case CRIT_OUI_ORG:
        case CRIT_MFG_ORG:
            arr.add(c.v.str ? c.v.str : "");
            break;
        default:
            break;
    }
}

bool RulesService::_saveUserRule(const Rule& r) {
    if (_loading) return false;         // bulk FS load in progress — never write back
    if (r.is_factory) return false;     // never overwrite factory files
    _ensureUserDir();

    char path[80];   // "/rules/user/" + name[56] + ".json"
    snprintf(path, sizeof(path), "%s/%s.json", DIR_USER, r.name);

    JsonDocument doc;
    doc["name"]  = r.name;
    doc["title"] = r.title;
    if (r.vibe != HAPTIC_PATTERN_COUNT) doc["vibe"] = vibePatternName(r.vibe);
    if (r.led  != LED_PATTERN_COUNT)    doc["led"]  = ledPatternName(r.led);
    if (r.alert_type != ALERT_TYPE_COUNT) {
        const char* t = "";
        switch (r.alert_type) {
            case ALERT_BLE:         t = "ble";  break;
            case ALERT_WIFI:        t = "wifi"; break;
            case ALERT_SYSTEM:      t = "sys";  break;
            case ALERT_BATTERY_LOW: t = "batt"; break;
            default: break;
        }
        doc["type"] = t;
    }
    if (r.action != RULE_ACTION_ALERT) {
        doc["action"] = (r.action == RULE_ACTION_PARTY) ? "party" : "alert";
    }

    // Group criteria by kind into per-kind JSON arrays. Order within each
    // kind preserves insertion order; kind order matches first-appearance.
    JsonArray criteria = doc["criteria"].to<JsonArray>();
    bool kind_emitted[CRIT_KIND_COUNT] = {false};
    for (uint16_t i = 0; i < r.criterion_count; i++) {
        const CriterionKind k = r.criteria[i].kind;
        if (k >= CRIT_KIND_COUNT || kind_emitted[k]) continue;
        const char* field = _kindToField(k);
        if (!field) continue;
        JsonObject obj = criteria.add<JsonObject>();
        JsonArray  arr = obj[field].to<JsonArray>();
        // Collect every criterion of this kind into one array.
        for (uint16_t j = i; j < r.criterion_count; j++) {
            if (r.criteria[j].kind == k) _appendCriterionValue(arr, r.criteria[j]);
        }
        kind_emitted[k] = true;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("[rules] FAIL: could not open %s for write\n", path);
        return false;
    }
    serializeJsonPretty(doc, f);
    f.close();
    return true;
}

bool RulesService::_deleteUserRuleFile(const char* name) {
    char path[80];   // "/rules/user/" + name[56] + ".json"
    snprintf(path, sizeof(path), "%s/%s.json", DIR_USER, name);
    if (!LittleFS.exists(path)) return false;
    return LittleFS.remove(path);
}

// ---------------------------------------------------------------------------
// Machine-readable I/O for the web companion
// ---------------------------------------------------------------------------

void RulesService::toJson(JsonArray rules) {
    for (uint16_t i = 0; i < _count; i++) {
        const Rule& r = _rules[i];
        JsonObject o = rules.add<JsonObject>();
        o["name"]    = r.name;
        o["title"]   = r.title;
        o["enabled"] = r.enabled;
        o["factory"] = r.is_factory;
        o["matches"] = r.match_count;
        if (r.vibe != HAPTIC_PATTERN_COUNT) o["vibe"] = vibePatternName(r.vibe);
        if (r.led  != LED_PATTERN_COUNT)    o["led"]  = ledPatternName(r.led);
        if (r.alert_type != ALERT_TYPE_COUNT) {
            const char* t = "";
            switch (r.alert_type) {
                case ALERT_BLE:         t = "ble";  break;
                case ALERT_WIFI:        t = "wifi"; break;
                case ALERT_SYSTEM:      t = "sys";  break;
                case ALERT_BATTERY_LOW: t = "batt"; break;
                default: break;
            }
            o["type"] = t;
        }
        if (r.action != RULE_ACTION_ALERT) {
            o["action"] = (r.action == RULE_ACTION_PARTY) ? "party" : "alert";
        }
        // Group atomic criteria back into per-kind arrays (same shape as files).
        JsonArray criteria = o["criteria"].to<JsonArray>();
        bool emitted[CRIT_KIND_COUNT] = {false};
        for (uint16_t a = 0; a < r.criterion_count; a++) {
            const CriterionKind k = r.criteria[a].kind;
            if (k >= CRIT_KIND_COUNT || emitted[k]) continue;
            const char* field = _kindToField(k);
            if (!field) continue;
            JsonArray arr = criteria.add<JsonObject>()[field].to<JsonArray>();
            for (uint16_t b = a; b < r.criterion_count; b++) {
                if (r.criteria[b].kind == k) _appendCriterionValue(arr, r.criteria[b]);
            }
            emitted[k] = true;
        }
    }
}

void RulesService::dumpJson(Print& out) {
    JsonDocument doc;
    toJson(doc.to<JsonArray>());
    serializeJson(doc, out);   // compact — one line
    out.println();
}

bool RulesService::saveRuleFromJson(const char* json) {
    if (!json) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;

    const char* name = doc["name"] | (const char*)nullptr;
    if (!name || !*name) return false;

    const int idx = _findRuleIdx(name);
    if (idx >= 0 && _rules[idx].is_factory) return false;   // never edit factory
    if (idx >= 0) deleteRule(name);                          // replace user rule

    if (!createRule(name)) return false;

    // Top-level fields. setRuleField no-ops on unknown values, so a bad value
    // just leaves the default rather than failing the whole save.
    const char* s;
    if ((s = doc["title"]  | (const char*)nullptr) && *s) setRuleField(name, "title",  s);
    if ((s = doc["vibe"]   | (const char*)nullptr) && *s) setRuleField(name, "vibe",   s);
    if ((s = doc["led"]    | (const char*)nullptr) && *s) setRuleField(name, "led",    s);
    if ((s = doc["type"]   | (const char*)nullptr) && *s) setRuleField(name, "type",   s);
    if ((s = doc["action"] | (const char*)nullptr) && *s) setRuleField(name, "action", s);

    // Criteria: each {field: [values...]} → CSV → addCriteria.
    for (JsonObject crit : doc["criteria"].as<JsonArray>()) {
        for (JsonPair kv : crit) {
            char csv[256];
            size_t n = 0;
            csv[0] = '\0';
            for (JsonVariant v : kv.value().as<JsonArray>()) {
                const char* val = v.as<const char*>();
                if (!val) continue;
                int w = snprintf(csv + n, sizeof(csv) - n, "%s%s", n ? "," : "", val);
                if (w < 0 || (size_t)w >= sizeof(csv) - n) break;   // safe truncate
                n += (size_t)w;
            }
            if (n) addCriteria(name, kv.key().c_str(), csv);
        }
    }

    if (doc["enabled"].is<bool>() && !doc["enabled"].as<bool>()) {
        setEnabled(name, false);
    }
    return true;
}


bool RulesService::_loadRuleFromFile(const char* path, bool is_factory) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[rules] %s: could not open\n", path);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[rules] %s: parse error: %s\n", path, err.c_str());
        return false;
    }

    const char* name = doc["name"] | (const char*)nullptr;
    if (!name || !*name) {
        Serial.printf("[rules] %s: missing 'name'\n", path);
        return false;
    }
    if (_findRuleIdx(name) >= 0) {
        return false;   // already loaded (factory beats user on name collision)
    }
    if (_count >= MAX_RULES) {
        Serial.printf("[rules] %s: MAX_RULES reached\n", path);
        return false;
    }

    // Use createRule for the slot allocation, then patch up factory flag.
    // (createRule saves an empty file to /rules/user; if we're loading a
    // factory rule that path is harmless but a wasted write. Skip the save
    // by inlining instead.)
    Rule& r = _rules[_count];
    memset(&r, 0, sizeof(Rule));
    strncpy(r.name,  name, sizeof(r.name)  - 1);
    strncpy(r.title, name, sizeof(r.title) - 1);
    r.vibe       = HAPTIC_PATTERN_COUNT;
    r.led        = LED_PATTERN_COUNT;
    r.alert_type = ALERT_TYPE_COUNT;
    r.action     = RULE_ACTION_ALERT;
    r.is_factory = is_factory;
    r.enabled    = true;
    _count++;

    // Title
    if (const char* t = doc["title"] | (const char*)nullptr) {
        strncpy(r.title, t, sizeof(r.title) - 1);
        r.title[sizeof(r.title) - 1] = '\0';
    }
    // Vibe / LED / type / action via setRuleField path bypassed: we'd loop
    // back into _saveUserRule. Apply directly here.
    if (const char* v = doc["vibe"] | (const char*)nullptr) {
        HapticPatternId p = vibePatternByName(v);
        if (p != HAPTIC_PATTERN_COUNT) r.vibe = p;
    }
    if (const char* v = doc["led"] | (const char*)nullptr) {
        LedPatternId p = ledPatternByName(v);
        if (p != LED_PATTERN_COUNT) r.led = p;
    }
    if (const char* v = doc["type"] | (const char*)nullptr) {
        if      (strcasecmp(v, "ble")    == 0 || strcasecmp(v, "bt") == 0) r.alert_type = ALERT_BLE;
        else if (strcasecmp(v, "wifi")   == 0)                             r.alert_type = ALERT_WIFI;
        else if (strcasecmp(v, "sys")    == 0 ||
                 strcasecmp(v, "system") == 0)                             r.alert_type = ALERT_SYSTEM;
        else if (strcasecmp(v, "batt")   == 0 ||
                 strcasecmp(v, "battery")== 0 ||
                 strcasecmp(v, "low")    == 0)                             r.alert_type = ALERT_BATTERY_LOW;
    }
    if (const char* v = doc["action"] | (const char*)nullptr) {
        if      (strcasecmp(v, "alert") == 0) r.action = RULE_ACTION_ALERT;
        else if (strcasecmp(v, "party") == 0) r.action = RULE_ACTION_PARTY;
    }

    // Criteria — each entry is { field: [values...] }. Iterate fields, build a
    // CSV, and add via the internal helper: no factory guard (this rule may be
    // factory) and no persistence (loading must never write back).
    JsonArray criteria = doc["criteria"].as<JsonArray>();
    for (JsonObject crit : criteria) {
        for (JsonPair kv : crit) {
            const char* field = kv.key().c_str();
            JsonArray   vals  = kv.value().as<JsonArray>();
            String csv;
            for (JsonVariant v : vals) {
                if (csv.length() > 0) csv += ",";
                csv += v.as<const char*>();
            }
            const int n = _addCriteriaToRule(r, field, csv.c_str());
            if (n < 0) {
                Serial.printf("[rules] %s: bad criterion field '%s'\n", path, field);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// NVS enabled-overlay
//
// Single packed blob containing newline-separated names of rules the user
// has disabled. On boot we read it and clear `enabled` for any matching
// rule; on every toggle we rewrite it. Avoids per-rule keys (NVS key length
// would constrain rule names) and survives FS reflash.
// ---------------------------------------------------------------------------

void RulesService::_applyEnabledOverlay() {
    Preferences prefs;
    // Open RW so the namespace is created on first boot. isKey() check
    // dodges the [E] log line that getString emits when the key is absent.
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly*/ false)) return;
    if (!prefs.isKey(NVS_KEY_DISABLED)) { prefs.end(); return; }
    String blob = prefs.getString(NVS_KEY_DISABLED, "");
    prefs.end();
    if (blob.isEmpty()) return;

    // Parse blob: \n-separated names. For each, if a matching rule exists,
    // flip its enabled flag off.
    int start = 0;
    while (start < (int)blob.length()) {
        int nl = blob.indexOf('\n', start);
        if (nl < 0) nl = blob.length();
        String name = blob.substring(start, nl);
        name.trim();
        if (name.length() > 0) {
            int idx = _findRuleIdx(name.c_str());
            if (idx >= 0) _rules[idx].enabled = false;
        }
        start = nl + 1;
    }
}

void RulesService::_persistEnabledOverlay() {
    String blob;
    for (uint16_t i = 0; i < _count; i++) {
        if (!_rules[i].enabled) {
            if (blob.length() > 0) blob += "\n";
            blob += _rules[i].name;
        }
    }
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly*/ false)) return;
    if (blob.isEmpty()) prefs.remove(NVS_KEY_DISABLED);
    else                prefs.putString(NVS_KEY_DISABLED, blob);
    prefs.end();
}
