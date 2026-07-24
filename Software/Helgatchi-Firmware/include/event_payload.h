#pragma once
#include <stdint.h>
#include "event_ids.h"

// --- Typed payload structs (max 8 bytes each) ---

struct ScanStatePayload    { uint8_t  domain; uint8_t active; };  // domain=ScanDomain (BLE/WiFi), active=0/1
struct EntityPayload       { uint32_t entity_id; };
struct AlertPayload        { uint16_t alert_id; uint8_t state; };
struct AlertCmdPayload     { uint16_t alert_id; };
struct SettingsPayload     { uint32_t mask; uint16_t version; };
struct SettingsSetPayload  { uint8_t  key; uint32_t value; };
struct PowerPayload        { uint8_t  state; };
struct BatteryPayload      { uint16_t mv; uint8_t pct; };
struct SleepCountPayload   { uint16_t seconds; };
struct RulePayload         { uint16_t rule_id; };
struct RulePackPayload     { uint8_t  pack_id; };
struct ObsBatchPayload     { uint8_t  count; };
struct MeshRulePayload     { uint8_t  origin_id[4]; uint8_t rule_id; };
struct LockonPayload       { uint8_t  domain; uint8_t mac[6]; uint8_t channel; };  // hunt target (channel: WiFi only)

// Single union covering all inline payloads — no heap, fixed 8 bytes.
union EventPayload {
    uint8_t            raw[8];
    ScanStatePayload   scan_state;
    EntityPayload      entity;
    AlertPayload       alert;
    AlertCmdPayload    alert_cmd;
    SettingsPayload    settings;
    SettingsSetPayload settings_set;
    PowerPayload       power;
    BatteryPayload     battery;
    SleepCountPayload  sleep_count;
    RulePayload        rule;
    RulePackPayload    rule_pack;
    ObsBatchPayload    obs_batch;
    MeshRulePayload    mesh_rule;
    LockonPayload      lockon;
    uint32_t           u32;
    uint16_t           u16;
    uint8_t            u8;
};

struct Event {
    EventId      id;
    uint8_t      _pad[2];   // alignment
    EventPayload data;
};
// sizeof(Event) == 12 bytes; queue of 64 = 768 bytes
