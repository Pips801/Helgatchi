#pragma once
#include "event_bus.h"
#include "scan_types.h"
#include "led_service.h"
#include "vibe_service.h"
#include "alerts_service.h"
#include <stdint.h>
#include <stddef.h>
#include <ArduinoJson.h>   // JsonArray for toJson()

class Print;   // Arduino Print (Serial) — for dumpJson

// ---------------------------------------------------------------------------
// Rules engine
//
// A Rule is a named bundle of match criteria plus the alert config to fire
// when any criterion hits. Every scan result drained from the scan ring is
// tested against every enabled rule; matching rules raise (or update) an
// alert via AlertsService with per-(rule, MAC) dedup.
//
// Phase 4 scope: rules live in PSRAM, created via serial commands. Phase 5
// adds LittleFS persistence (load on boot, save on mutation).
//
// Match semantics (decided in planning):
//   - Within a rule, all criteria are OR'd (any hit fires the rule).
//   - Each rule can carry many values per criterion; each value is its own
//     atomic match.
//   - Across rules, each match raises its own alert.
//   - Dedup is per-(rule, MAC): re-firing on the same device updates the
//     existing alert's last_seen instead of stacking new ones.
//
// `oui_org` / `mfg_org` are pattern kinds (CRIT_OUI_ORG / CRIT_MFG_ORG)
// evaluated at match time: the sighting's vendor name is resolved once per
// sighting (a bsearch, reused across every rule) and the pattern tested against
// it. Deferring keeps boot cheap — no vendor-table scan at load — for a tiny
// per-sighting cost.
// ---------------------------------------------------------------------------

enum CriterionKind : uint8_t {
    CRIT_OUI,              // 24-48 bit MAC prefix match against scan.mac (MA-L/M/S)
    CRIT_MAC,              // exact 6-byte MAC
    CRIT_MFG,              // 16-bit BT SIG company id (scan.mfg_id)
    CRIT_SERVICE,          // 128-bit BLE service UUID, matched against any of scan.service_uuids
    CRIT_NAME_MATCH,       // pattern (see PatShape) vs scan.name, any domain
    CRIT_SSID_MATCH,       // pattern vs scan.name, gated to SCAN_WIFI
    CRIT_OUI_ORG,          // pattern vs the MAC-OUI vendor name, resolved at match time
    CRIT_MFG_ORG,          // pattern vs the mfg-id company name, resolved at match time
    CRIT_IE_SIG,           // pattern vs scan.ie_sig (802.11 IE fingerprint), gated to SCAN_WIFI
    CRIT_KIND_COUNT,
};

// Classified shape of a name/ssid/*_org pattern, decided once at add time.
// Every shape but PAT_REGEX runs as a plain case-insensitive string compare on
// the hot path (covers every shipped rule); only PAT_REGEX invokes re_lite.
// The literal "core" for the fast-path shapes is the substring [off, off+len)
// of the stored pattern string (i.e. the pattern with its .* affixes skipped).
// See docs/WRITING_RULES.md.
enum PatShape : uint8_t {
    PAT_EXACT,     // literal   → strcasecmp
    PAT_CONTAINS,  // .*core.*  → case-insensitive substring
    PAT_PREFIX,    // core.*    → case-insensitive starts-with
    PAT_SUFFIX,    // .*core    → case-insensitive ends-with
    PAT_REGEX,     // otherwise → re_lite_full_match(pattern, name)
};

// The pattern-valued kinds: NAME/SSID (vs the device name), OUI_ORG/MFG_ORG
// (vs the resolved vendor name), and IE_SIG (vs the WiFi frame's IE
// fingerprint). All store a pattern in v.str plus a classified shape.
struct Criterion {
    CriterionKind kind;
    PatShape      pat_shape;         // valid for the pattern kinds (NAME/SSID/OUI_ORG/MFG_ORG/IE_SIG)
    uint8_t       pat_off;           // literal-core offset within v.str (fast-path shapes)
    uint8_t       pat_len;           // literal-core length
    union {
        // OUI prefix, 24-48 bits (MA-L / MA-M / MA-S). `bytes` is MSB-first;
        // an odd `nibbles` count keeps its trailing nibble in the high half of
        // bytes[nibbles/2], low half zeroed. Match is a nibble-wise prefix
        // compare against scan.mac. nibbles is 6..12.
        struct { uint8_t bytes[6]; uint8_t nibbles; } oui;
        uint16_t    mfg_id;
        uint8_t     mac[6];
        uint8_t     uuid[16];
        const char* str;             // owned by the criterion; heap_caps_malloc PSRAM.
                                      // For the pattern kinds: the verbatim pattern.
    } v;
};

enum RuleAction : uint8_t {
    RULE_ACTION_ALERT = 0,           // raise/update via AlertsService
    RULE_ACTION_PARTY,               // stretch goal — emits EV_PARTY_MODE (Phase 6)
    RULE_ACTION_COUNT,
};

struct Rule {
    char            name[56];        // unique identifier, lowercase a-z 0-9 underscore.
                                     // 55 usable — ceiling comes from LittleFS's 64-char
                                     // filename segment ("<name>.json" ≤ 63);
                                     // AlertRecord::identifier must fit name+':'+12 hex MAC
    char            title[40];       // alert title shown in UI
    char            tags[4][24];      // up to 4 tags per rule (e.g. "Smart Home", "Audio")
    uint8_t         tag_count;
    HapticPatternId vibe;             // HAPTIC_PATTERN_COUNT = service default
    LedPatternId    led;              // LED_PATTERN_COUNT    = service default
    AlertType       alert_type;       // ALERT_TYPE_COUNT     = infer from scan domain
    RuleAction      action;
    bool            is_factory;       // false = user-editable; Phase 5 distinguishes
    bool            enabled;
    Criterion*      criteria;         // PSRAM, realloc'd as needed
    uint16_t        criterion_count;
    uint16_t        criterion_cap;
    uint32_t        match_count;      // per-rule firings since boot (informational)
};

class RulesService : public IEventHandler {
public:
    static constexpr uint16_t MAX_RULES        = 64;
    static constexpr uint16_t MAX_CRITERIA     = 256;   // hard cap per rule, protects against runaway org expansion
    static constexpr size_t   DRAIN_BATCH      = 16;    // scans processed per tick()

    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Wipe in-memory state and re-read /rules/factory + /rules/user from
    // LittleFS. Preserves NVS enable overlay (user enabled-state survives).
    // Returns number of rules loaded.
    uint16_t reloadFromFs();

    // Factory reset: delete every user ruleset file, clear the NVS
    // enabled-overlay, and reload — leaves the factory set only, everything
    // disabled (out-of-box state).
    void factoryReset();

    // --- Mutation API (serial commands today; JSON parser uses these in Phase 5) ---

    // Create an empty rule with the given name. Returns false on duplicate
    // name or if MAX_RULES is full. Defaults: title=name, alert_type=infer,
    // vibe/led=service default, action=alert, enabled=true.
    bool createRule(const char* name);

    // Set one of the top-level fields on an existing rule. `field` is one of:
    // title, vibe, led, type, action. `value` is interpreted per field.
    bool setRuleField(const char* name, const char* field, const char* value);

    // Add criteria. `field` is the rule-file field name (oui, mac, mfg,
    // service, name, ssid, oui_org, mfg_org, ie_sig). The pattern fields
    // (name/ssid/oui_org/mfg_org/ie_sig) take case-insensitive full-match
    // patterns (see PatShape / docs/WRITING_RULES.md).
    // `values_csv` is one or more comma-separated values for that field;
    // each becomes its own atomic criterion. Returns the count of criteria
    // added, or -1 on parse error / invalid pattern.
    int addCriteria(const char* name, const char* field, const char* values_csv);

    // Remove the Nth criterion (0-indexed in arrival order).
    bool removeCriterion(const char* name, uint16_t idx);

    // Delete a rule entirely, freeing its strings + criterion array.
    bool deleteRule(const char* name);

    // Enable / disable. Rules load disabled; the NVS overlay stores the set
    // of enabled names, so first boot / post-erase / factory reset all mean
    // "nothing enabled" until the user opts in.
    bool setEnabled(const char* name, bool enabled);

    // Which radios a rule's criteria actually depend on, as a mask of
    // (1 << SCAN_BLE) | (1 << SCAN_WIFI) — the same bit layout as SKEY_SCAN_MODE
    // (bit 0 = BLE, bit 1 = WiFi), so the two can be AND'ed directly.
    //
    // Counts only DOMAIN-SPECIFIC criteria: `ssid`/`ie_sig` are gated to WiFi at
    // match time, and `mfg`/`mfg_org`/`service` can only ever match a BLE
    // advertisement (mfg_id is 0 and there are no service UUIDs on a WiFi frame).
    // `oui`/`mac`/`name`/`oui_org` match either radio and contribute nothing.
    //
    // Returns 0 for a rule that is entirely radio-agnostic, or an unknown name.
    // Lets the UI warn that enabling a rule won't achieve much while the radio it
    // keys on is switched off.
    uint8_t ruleRadioMask(const char* name) const;

    // --- Tag API ---
    bool hasTag(const Rule& r, const char* tag) const;
    bool isTagEnabled(const char* tag) const;
    bool setTagEnabled(const char* tag, bool enabled);

    // ruleRadioMask, unioned across every rule carrying `tag` — enabling a tag
    // turns all of them on at once, so the tag depends on a radio if any one of
    // its rules does. 0 for an unknown tag or an entirely radio-agnostic set.
    uint8_t tagRadioMask(const char* tag) const;
    uint16_t getUniqueTags(char out_tags[][24], uint16_t max_tags) const;

    // --- Machine-readable I/O for the web companion ---

    // Fill `out` with every rule as a JSON object (file fields + runtime state:
    // enabled / factory / matches). Lets callers embed rules in a larger doc.
    void toJson(JsonArray out);

    // Serialize toJson() as one compact line to `out` (the `rule dump` command).
    void dumpJson(Print& out);

    // Create or replace a USER rule from a full rule JSON object (same shape as
    // a rule file). Rejects editing a factory rule. Auto-persists. Returns
    // false on parse error, missing name, or a factory-name collision.
    bool saveRuleFromJson(const char* json);

    // --- Read API ---

    uint16_t    count() const { return _count; }         // number of loaded rulesets
    const Rule* get(uint16_t idx) const;
    const Rule* find(const char* name) const;

    // Total match rules (criteria) summed across every loaded ruleset.
    uint32_t    totalRules() const {
        uint32_t n = 0;
        for (uint16_t i = 0; i < _count; i++) n += _rules[i].criterion_count;
        return n;
    }

    uint32_t totalMatches() const { return _match_count; }
    uint32_t lostScans()    const { return _lost_scans; }
    uint32_t ringReadPos()  const { return _ring_read_pos; }

private:
    EventBus* _bus = nullptr;
    Rule      _rules[MAX_RULES] = {};
    uint16_t  _count            = 0;

    uint32_t  _ring_read_pos    = 0;   // ScanService monotonic counter
    uint32_t  _match_count      = 0;
    uint32_t  _lost_scans       = 0;
    bool      _loading          = false;  // true during a bulk FS load — suppresses
                                          // _saveUserRule so loading a rule never
                                          // writes it back (esp. factory rules)

    // Mutation helpers
    // Post EV_RULES_CHANGED so UI consumers re-read the ruleset list. Called
    // after every successful mutation; suppressed during bulk FS loads.
    void     _notifyChanged();
    int      _findRuleIdx(const char* name) const;
    bool     _appendCriterion(Rule& r, const Criterion& c);
    bool     _ensureCapacity(Rule& r, uint16_t need);
    void     _freeCriterion(Criterion& c);
    void     _freeRuleContents(Rule& r);
    // Parse one field=csv into criteria on `r`. No factory guard, no persist —
    // the public addCriteria() wraps this with both; the loader calls it raw.
    int      _addCriteriaToRule(Rule& r, const char* field, const char* values_csv);

    // Match path
    void     _matchScan(const ScanResult& scan);
    // `oui_org` / `mfg_org` are the vendor names resolved once for this sighting
    // (nullptr if unknown), passed in so CRIT_OUI_ORG / CRIT_MFG_ORG don't each
    // re-run the bsearch.
    bool     _criterionMatches(const Criterion& c, const ScanResult& s,
                               const char* oui_org, const char* mfg_org) const;
    void     _fire(Rule& r, const ScanResult& s);

    // Persistence (FS + NVS overlay)
    void     _loadDir(const char* dir_path, bool is_factory);
    bool     _loadRuleFromFile(const char* path, bool is_factory);
    bool     _saveUserRule(const Rule& r);                 // writes /rules/user/<name>.json
    bool     _deleteUserRuleFile(const char* name);
    void     _ensureUserDir();                              // mkdir /rules /rules/user if missing
    void     _applyEnabledOverlay();                        // read NVS blob, mark rules disabled
    void     _persistEnabledOverlay();                      // rewrite NVS blob from current state
};

extern RulesService g_rules;

// Parse a service UUID — short form ("180F", "0x180F", promoted via the BLE
// base UUID) or full dashed 128-bit form — into 16-byte LSB-first wire order,
// byte-comparable to ScanResult.service_uuids. Used by rule criteria and
// `scan inject service=` so the two can't drift.
bool parseServiceUuid(const char* s, uint8_t out[16]);
