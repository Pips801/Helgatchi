#pragma once
#include "event_bus.h"
#include "scan_types.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// Scan service
//
// Owns the producer/consumer plumbing between scan callbacks (BLE / WiFi)
// and the rules engine + device-list UI. Two parallel data structures:
//
//   ring buffer       — every scan event, in order. RulesService drains it
//                       on tick(). Sized for ~5 s of slack at a heavy BLE
//                       advertisement rate; oldest entries are overwritten
//                       if a consumer falls behind.
//
//   seen-devices map  — one entry per unique MAC, holding the latest data
//                       for that device. DeviceListScreen reads this on
//                       demand. Linear-probed insertion order — when full,
//                       the entry with the oldest last-seen time is evicted.
//
// Both structures live in PSRAM (allocated in begin()). The ring uses a
// monotonic write counter so multiple consumers can each track their own
// read position without coordinating with the producer.
//
// Phase 1: NO real scan callbacks yet. The only producer is the `scan
// inject` serial command for testing. Real NimBLE + WiFi wiring lands in
// Phase 6.
// ---------------------------------------------------------------------------

class ScanService {
public:
    static constexpr size_t RING_CAPACITY = 256;
    static constexpr size_t SEEN_CAPACITY = 256;

    void begin(EventBus& bus);

    // Producer — called by scan callbacks and `scan inject`. Pushes to the
    // ring (overwriting oldest entry if full) and upserts into the seen-
    // devices map keyed by MAC. Always succeeds; no return value.
    void publish(const ScanResult& r);

    // Consumer — drain new ring entries from the caller's last-known
    // position. `*read_pos` is monotonic (caller persists it between
    // calls; initialize to 0 or to writePos() at startup). Copies up to
    // `max` entries into `out` and advances `*read_pos` past them.
    // Returns the number of entries copied.
    //
    // If the write head has lapped the caller (more than RING_CAPACITY
    // ahead), entries were lost. `*lost_out` (if non-null) is incremented
    // by the loss count and `*read_pos` fast-forwards to the oldest still-
    // available entry before copying resumes.
    size_t drain(uint32_t* read_pos, ScanResult* out, size_t max,
                 uint32_t* lost_out = nullptr);

    // Current monotonic write position. Useful for "start draining from
    // now, ignore backlog" semantics at consumer startup.
    uint32_t writePos() const { return _write_pos; }

    // Seen-devices iteration. Const access; mutation goes through publish().
    // Indices are stable across publish() calls EXCEPT when an entry is
    // evicted (only happens when the map is full and a new MAC appears).
    size_t            seenCount() const { return _seen_count; }
    const ScanResult& seenAt(size_t i) const { return _seen[i]; }

    // Look up a specific device in the seen map by domain + MAC. Returns the
    // latest ScanResult for it, or nullptr if it's not (or no longer) present.
    // Used by the device-detail view to re-fetch live data by identity across
    // scans, independent of shifting seen-map indices.
    const ScanResult* findSeen(uint8_t domain, const uint8_t mac[6]) const;

    // Force a sighting into the seen map even when the randomized-MAC noise
    // filter would have dropped it. RulesService calls this when a rule fires:
    // a device that matched a rule is not noise, and the alerts screen needs
    // findSeen() to resolve for the alert card's device-detail overlay. Called
    // again on every re-fire, which keeps the entry's last-seen fresh so it
    // can't become the eviction victim while the device is still present.
    //
    // Retained entries stay OUT of the browsable device list — that half of the
    // filter moved to the list itself (see noiseSuppressed), so
    // SKEY_IGNORE_RANDOMIZED_MACS still means exactly what it says.
    void retain(const ScanResult& r);

    // True when the randomized-MAC noise filter suppresses this sighting: a
    // nameless BLE RPA/NRPA while SKEY_IGNORE_RANDOMIZED_MACS is on.
    //
    // Applied at two layers for two different reasons, and shared so the two
    // can't drift: _updateSeen won't INSERT these (protects the map's slots
    // from RPA churn evicting real devices), and the device list won't RENDER
    // the ones retain() forced in anyway.
    static bool noiseSuppressed(const ScanResult& r);

    // Debug / test — wipe ring and seen map. Does NOT reset the monotonic
    // write counter, so live consumers won't see a backwards jump.
    void clear();

    // Count of new devices kept out of the seen map by the randomized-MAC noise
    // filter since boot (nameless RPA/NRPA BLE). They still hit the ring, so
    // rules/alerts see them, and one that fires a rule is added back by
    // retain(). Perf telemetry only. Not reset by clear().
    uint32_t noiseFiltered() const { return _noise_filtered; }

private:
    EventBus*   _bus       = nullptr;
    ScanResult* _ring      = nullptr;     // PSRAM, RING_CAPACITY entries
    uint32_t    _write_pos = 0;           // monotonic, mod CAPACITY for index

    ScanResult* _seen       = nullptr;    // PSRAM, SEEN_CAPACITY entries
    size_t      _seen_count = 0;

    uint32_t    _noise_filtered = 0;      // nameless randomized BLE dropped from the seen map

    // Upsert by MAC into the seen map. New MAC appends (or evicts oldest
    // last-seen entry when full); existing MAC updates in place.
    // `bypass_noise_filter` is retain()'s path — see noiseSuppressed.
    void _updateSeen(const ScanResult& r, bool bypass_noise_filter);
};

extern ScanService g_scan_service;
