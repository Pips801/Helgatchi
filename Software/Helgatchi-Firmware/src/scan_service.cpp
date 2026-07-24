#include "scan_service.h"
#include "settings_service.h"
#include "settings_keys.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

// MUST NOT be named `g_scan`: the WiFi/net80211 ROM driver defines an ABSOLUTE
// symbol `g_scan` (its own scan-state struct) at a fixed DRAM address, and a
// plain `g_scan` here collides with it — the linker drops this object right on
// top of the WiFi driver's global table (g_scan / net80211_funcs /
// s_encap_amsdu_func / …), so WiFi init and this object silently overwrite each
// other's memory. That collision was the root of ALL the WiFi-scan corruption.
// Keep a name that isn't a WiFi/ROM symbol; audit with:
//   xtensa-esp32s3-elf-nm firmware.elf | awk '$2=="A"{print $3}' | grep '^g_'
ScanService g_scan_service;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ScanService::begin(EventBus& bus) {
    _bus = &bus;

    // Both buffers in PSRAM — internal SRAM is tight and these structures
    // are large (ring ~30 KB, seen map ~30 KB at current ScanResult size).
    // Access latency is fine for our drain cadence (~once per tick).
    if (!_ring) {
        _ring = (ScanResult*)heap_caps_malloc(
            sizeof(ScanResult) * RING_CAPACITY, MALLOC_CAP_SPIRAM);
    }
    if (!_seen) {
        _seen = (ScanResult*)heap_caps_malloc(
            sizeof(ScanResult) * SEEN_CAPACITY, MALLOC_CAP_SPIRAM);
    }

    if (!_ring || !_seen) {
        Serial.println("[scan] FATAL: PSRAM alloc failed");
        // Degrade gracefully — publish() will no-op on null buffers.
        return;
    }

    memset(_ring, 0, sizeof(ScanResult) * RING_CAPACITY);
    memset(_seen, 0, sizeof(ScanResult) * SEEN_CAPACITY);
    _write_pos  = 0;
    _seen_count = 0;
}

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------

void ScanService::publish(const ScanResult& r) {
    if (!_ring || !_seen) return;

    _ring[_write_pos % RING_CAPACITY] = r;
    _write_pos++;

    _updateSeen(r);
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------

size_t ScanService::drain(uint32_t* read_pos, ScanResult* out, size_t max,
                          uint32_t* lost_out) {
    if (!_ring || !read_pos || !out || max == 0) return 0;

    // Unsigned subtraction handles 32-bit wraparound correctly as long as
    // the real gap fits in 31 bits — guaranteed since we cap at RING_CAPACITY.
    const uint32_t available = _write_pos - *read_pos;
    if (available == 0) return 0;

    // Caller fell more than RING_CAPACITY behind — older entries are gone.
    // Fast-forward to the oldest still-readable position.
    if (available > RING_CAPACITY) {
        const uint32_t lost = available - RING_CAPACITY;
        if (lost_out) *lost_out += lost;
        *read_pos = _write_pos - RING_CAPACITY;
    }

    const uint32_t remaining = _write_pos - *read_pos;
    const size_t   to_copy   = (remaining < max) ? remaining : max;

    for (size_t i = 0; i < to_copy; i++) {
        out[i] = _ring[(*read_pos + i) % RING_CAPACITY];
    }
    *read_pos += to_copy;
    return to_copy;
}

const ScanResult* ScanService::findSeen(uint8_t domain, const uint8_t mac[6]) const {
    if (!_seen) return nullptr;
    for (size_t i = 0; i < _seen_count; i++) {
        if (_seen[i].domain == domain && memcmp(_seen[i].mac, mac, 6) == 0) {
            return &_seen[i];
        }
    }
    return nullptr;
}

void ScanService::clear() {
    if (_ring) memset(_ring, 0, sizeof(ScanResult) * RING_CAPACITY);
    if (_seen) memset(_seen, 0, sizeof(ScanResult) * SEEN_CAPACITY);
    _seen_count = 0;
    // Intentionally NOT resetting _write_pos — any live consumer is
    // tracking its own read_pos against our monotonic counter and would
    // see a backwards jump as catastrophic data loss.
}

// ---------------------------------------------------------------------------
// Seen-devices map
// ---------------------------------------------------------------------------

void ScanService::_updateSeen(const ScanResult& r) {
    // Existing entry by MAC → update in place. Domain is part of the key
    // because BLE and WiFi MAC namespaces overlap (random/synthetic MACs).
    for (size_t i = 0; i < _seen_count; i++) {
        if (_seen[i].domain == r.domain &&
            memcmp(_seen[i].mac, r.mac, sizeof(r.mac)) == 0) {
            const uint32_t first = _seen[i].first_seen_ms;   // preserve original sighting
            _seen[i] = r;
            _seen[i].first_seen_ms = first;
            return;
        }
    }

    // New device. Optionally drop nameless randomized BLE addresses before they
    // ever enter the seen map: keeps the map small, stops RPA/NRPA churn from
    // evicting real named devices, and keeps them off the device list. The ring
    // write in publish() already happened before this call, so rules/alerts
    // still fire on them — this only suppresses the browsable list entry.
    if (r.domain == SCAN_BLE && macTypeIsRandom(r.mac_type) && r.name[0] == '\0' &&
        g_settings.getBool(SKEY_IGNORE_RANDOMIZED_MACS)) {
        _noise_filtered++;
        return;
    }

    // New entry — append, or evict the oldest-last-seen if full.
    if (_seen_count < SEEN_CAPACITY) {
        _seen[_seen_count] = r;
        _seen[_seen_count].first_seen_ms = r.timestamp_ms;
        _seen_count++;
        return;
    }

    size_t   oldest_idx = 0;
    uint32_t oldest_ms  = _seen[0].timestamp_ms;
    for (size_t i = 1; i < _seen_count; i++) {
        if (_seen[i].timestamp_ms < oldest_ms) {
            oldest_ms  = _seen[i].timestamp_ms;
            oldest_idx = i;
        }
    }
    _seen[oldest_idx] = r;
    _seen[oldest_idx].first_seen_ms = r.timestamp_ms;
}
