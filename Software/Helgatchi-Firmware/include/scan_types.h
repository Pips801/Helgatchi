#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Scan result — one device sighting from BLE or WiFi.
//
// Produced by ScanService (NimBLE callbacks + WiFi scan) and consumed by:
//   - RulesService          drains the ring buffer, matches against loaded
//                           rulesets, raises alerts on hit.
//   - DeviceListScreen      reads the dedup'd seen-devices map for browsing.
//
// Stays a POD struct so it can sit in PSRAM ring storage and be memcpy'd
// freely. No pointers, no virtuals.
// ---------------------------------------------------------------------------

enum ScanDomain : uint8_t {
    SCAN_BLE  = 0,
    SCAN_WIFI = 1,
};

// Frame class for a sighting: 802.11 management subtype for WiFi (set by the
// promiscuous sniffer) or a BLE advertisement. FRAME_UNKNOWN is the default for
// non-classified rows (e.g. injected tests).
enum FrameKind : uint8_t {
    FRAME_UNKNOWN   = 0,   // not a classified frame
    FRAME_BEACON    = 1,   // 802.11 beacon (type 0, subtype 8) — AP announcement
    FRAME_PROBE_REQ = 2,   // 802.11 probe request (type 0, subtype 4) — STA seeking APs
    FRAME_BLE_ADV   = 3,   // BLE advertisement (everything we observe over BLE)
};

// Short label for `scan list` / debug / the device-detail popup. Never null.
inline const char* frameKindName(uint8_t k) {
    switch (k) {
        case FRAME_BEACON:    return "beacon";   // WiFi AP
        case FRAME_PROBE_REQ: return "probe";    // WiFi client
        case FRAME_BLE_ADV:   return "adv";      // BLE advertisement
        default:              return "-";
    }
}

// BLE MAC address classification, derived from the advertised address type
// plus (for random addresses) the two most-significant bits of the address.
// WiFi results and injected test rows are MAC_TYPE_UNKNOWN.
enum MacAddrType : uint8_t {
    MAC_TYPE_UNKNOWN = 0,   // not classified (WiFi, injected, or pre-classification)
    MAC_TYPE_PUBLIC,        // public IEEE-registered address — static, vendor-owned
    MAC_TYPE_RANDOM_STATIC, // random static — fixed until the device re-randomizes (power cycle)
    MAC_TYPE_RPA,           // resolvable private address — rotates (~15 min), resolvable with the IRK
    MAC_TYPE_NRPA,          // non-resolvable private address — fully random, not tied to an identity
    MAC_TYPE_RANDOM_OTHER,  // random with the reserved (0b10) sub-type prefix
};

// True for BLE address types that rotate with no persistent identity: RPA
// (rotates ~15 min), NRPA (per-session random), and the reserved random
// subtype. Random-static is deliberately excluded — it's stable until the
// device power-cycles, so it's a usable session identifier rather than
// ephemeral noise. Public and MAC_TYPE_UNKNOWN (WiFi / injected) are not
// random. Used by the device-list noise filter (SKEY_IGNORE_RANDOMIZED_MACS).
inline bool macTypeIsRandom(uint8_t t) {
    return t == MAC_TYPE_RPA || t == MAC_TYPE_NRPA || t == MAC_TYPE_RANDOM_OTHER;
}

// Short label for `scan list` / debug output. Never null. RPA and NRPA both
// surface as "random" — the user-facing split is static / rotating / random.
inline const char* macTypeName(uint8_t t) {
    switch (t) {
        case MAC_TYPE_PUBLIC:        return "static";    // public IEEE address
        case MAC_TYPE_RANDOM_STATIC: return "rotating";  // random static
        case MAC_TYPE_RPA:           return "random";    // RPA
        case MAC_TYPE_NRPA:          return "random";    // NRPA
        case MAC_TYPE_RANDOM_OTHER:  return "random";    // reserved random subtype
        default:                     return "-";
    }
}

struct ScanResult {
    uint8_t  domain;            // ScanDomain
    uint8_t  mac[6];
    uint8_t  mac_type;          // MacAddrType — BLE address classification; MAC_TYPE_UNKNOWN for WiFi
    int8_t   rssi;
    uint8_t  channel;           // WiFi primary channel (1..14). 0 for BLE / unknown. Used by WiFi lock-on.
    uint8_t  frame_kind;        // FrameKind — 802.11 frame class (WiFi promiscuous sniffer). FRAME_UNKNOWN for BLE.
    char     name[32];          // BLE adv name OR WiFi SSID. NUL-terminated; truncated if longer.
    // Observed 802.11 IE fingerprint for WiFi frames: comma-separated tag list
    // with vendor (tag 221) IEs expanded to "221:<hex>". Empty for BLE / frames
    // with no notable IEs. Matched by the CRIT_IE_SIG rule criterion. See
    // docs/WRITING_RULES.md and the Flock detector in data/rules/factory/.
    char     ie_sig[64];
    uint16_t mfg_id;            // BT SIG company ID (BLE). 0 = none. Unused for WiFi.
    uint8_t  service_count;     // number of populated entries in service_uuids
    // 128-bit UUIDs in BLE wire order (LSB first). 16/32-bit UUIDs from
    // NimBLE are promoted via the BLE base UUID at scan-emit time so
    // rule matching has a single canonical form.
    uint8_t  service_uuids[4][16];
    uint32_t timestamp_ms;      // millis() at the scan callback (i.e. last seen)
    uint32_t first_seen_ms;     // millis() first added to the seen map. Maintained
                                // by ScanService::_updateSeen; unused in the ring.
};

static_assert(sizeof(ScanResult) <= 192, "ScanResult bloating — reduce service slots, ie_sig, or name");
