#pragma once
#include "event_bus.h"
#include "scan_types.h"
#include "admin_types.h"
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Scan engine — owns the BLE (and later, WiFi) radios and feeds ScanResults
// into ScanService. NimBLE callbacks run on the BLE host task; results are
// marshalled to the main loop via a FreeRTOS queue, so ScanService.publish()
// only ever runs from one thread.
//
// Triggered by:
//   CMD_SCAN_START — PowerManager opens a scan window
//   CMD_SCAN_STOP  — scan window closes
//
// SKEY_SCAN_MODE bit 0 gates BLE (bit 1 reserved for WiFi). Settings
// changes that touch SMASK_SCAN re-apply parameters on the next start.
// ---------------------------------------------------------------------------

class ScanEngine : public IEventHandler {
public:
    // Queue depth — how many BLE callbacks can land before tick() drains.
    // 50 advertisements/sec is typical; 64 gives just over a second of slack.
    static constexpr size_t QUEUE_DEPTH = 64;
    // Admin command frames are rare; a small queue drop-on-full is benign since
    // frames retransmit throughout the sender's broadcast burst.
    static constexpr size_t ADMIN_QUEUE_DEPTH = 8;

    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Idempotent one-time NimBLE init. Owned here (the only NimBLEDevice::init
    // in the firmware); AdminService calls this before it advertises so scan and
    // advertise share one init. Safe to call repeatedly. May block ~tens of ms
    // on the very first call while the host syncs — keep it on the main loop.
    void ensureBle();

    // Pop one received admin command frame (filled by the NimBLE callback).
    // Returns false when the admin queue is empty. Drained by AdminService::tick.
    bool popAdminFrame(AdminFrame& out);

    // Suspend all scanning (BLE now, WiFi when it lands) while true, so an admin
    // controller can dedicate the radio to advertising. Stops any live scan
    // immediately; releasing resumes it if we're inside a scan window. AdminService
    // holds this true only while it is broadcasting a command.
    void setScanInhibited(bool inhibit);
    bool scanInhibited() const { return _scan_inhibited; }

    // Lock-on ("foxhunt") state, read by FoxhuntingScreen's poll timer. While a
    // lock-on is active the normal duty-cycle scan is torn down and the radio is
    // dedicated to tracking ONE target's RSSI as fast as possible: BLE runs a
    // continuous active scan with duplicates enabled; WiFi sits in promiscuous
    // mode on the target's channel. Everything that isn't the target is dropped
    // (no ring / seen-map / rules churn). Driven by CMD_SCAN_LOCKON_START/STOP.
    bool     lockonActive()     const { return _lockon_active; }
    bool     lockonHasHit()     const { return _lockon_have; }        // ≥1 target sighting since start
    int8_t   lockonRssi()       const { return _lockon_rssi; }        // last target RSSI (valid iff lockonHasHit)
    uint32_t lockonLastSeenMs() const { return _lockon_last_ms; }     // millis() of last target sighting

    // Stats counters — incremented in the BLE callback (queue full) and tick
    // (drained). For diagnostics via a future serial command.
    uint32_t callbacks()      const { return _cb_count; }
    uint32_t queueOverflows() const { return _q_overflow; }
    uint32_t published()      const { return _pub_count; }
    bool     bleActive()      const { return _ble_scanning; }
    bool     wifiActive()     const { return _wifi_scanning; }
    uint32_t wifiScans()      const { return _wifi_scan_count; }
    uint32_t wifiResults()    const { return _wifi_result_count; }

    // True while an async WiFi sweep is in flight (esp_wifi busy). PowerManager
    // folds this into its pre-sleep drain gate so we never deep-sleep mid-sweep
    // (before CMD_SCAN_STOP has been dispatched to abort it).
    bool     wifiBusy()       const { return _wifi_scan_inflight; }

    // Number of ScanResults sitting in the BLE-host→main-loop queue. Used
    // by PowerManager to decide when post-scan-stop drain is complete.
    size_t   queueDepth()     const;

private:
    EventBus* _bus = nullptr;
    void*     _queue = nullptr;       // QueueHandle_t, opaque to keep FreeRTOS out of the header
    void*     _admin_queue = nullptr; // QueueHandle_t for received admin command frames

    bool      _ble_initialized = false;
    bool      _ble_scanning    = false;
    bool      _in_scan_window  = false;   // tracks CMD_SCAN_START / CMD_SCAN_STOP
    bool      _scan_inhibited  = false;   // admin broadcast owns the radio while true

    bool      _wifi_initialized   = false;
    bool      _wifi_scanning      = false;   // WiFi is the active radio this phase
    bool      _wifi_scan_inflight = false;   // the single per-phase WiFi.scanNetworks() is running

    // Lock-on / foxhunt. When active the normal phase machine is bypassed; the
    // callbacks (BLE onResult, WiFi promiscuous rx) update _lockon_rssi /
    // _lockon_last_ms for the target only. RSSI/last-seen are written from radio
    // task callbacks and read from the main loop — 8/32-bit aligned scalars, so
    // the reads/writes are atomic on Xtensa; no lock needed.
    bool      _lockon_active   = false;
    uint8_t   _lockon_domain   = 0;
    uint8_t   _lockon_mac[6]   = {0};
    uint8_t   _lockon_channel  = 0;      // WiFi target channel (promiscuous pin)
    volatile int8_t   _lockon_rssi    = 0;
    volatile uint32_t _lockon_last_ms = 0;
    volatile bool     _lockon_have    = false;
    bool      _wifi_promisc    = false;  // esp_wifi promiscuous mode currently on

    // Intra-window radio phase sequence. Radios are time-multiplexed, not
    // concurrent: each enabled domain owns the radio for the full scan duration
    // in turn, so BLE and WiFi never share airtime. _scan_seq lists the enabled
    // domains in order (BLE then WiFi); tick() advances _seq_idx at each phase
    // boundary (_phase_dur_ms, taken from PowerManager::scanDurationS()). The
    // last phase runs until CMD_SCAN_STOP closes the window.
    uint8_t   _scan_seq[2]    = {0, 0};
    uint8_t   _seq_len        = 0;
    uint8_t   _seq_idx        = 0;
    uint32_t  _phase_start_ms = 0;
    uint32_t  _phase_dur_ms   = 0;

    // Stats
    uint32_t  _cb_count          = 0;
    uint32_t  _q_overflow        = 0;
    uint32_t  _pub_count         = 0;   // total ScanResults published (BLE + WiFi)
    uint32_t  _wifi_scan_count   = 0;   // completed WiFi sweeps since boot
    uint32_t  _wifi_result_count = 0;   // WiFi APs published since boot

    void _startBle();
    void _stopBle();

    // Lock-on radio control. Entered on CMD_SCAN_LOCKON_START (after tearing down
    // any normal scan), left on CMD_SCAN_LOCKON_STOP.
    void _startLockon();
    void _stopLockon();
    void _startBleLockon();    // continuous active scan, duplicates on
    void _startWifiLockon();   // promiscuous mode pinned to _lockon_channel
    void _stopWifiLockon();

    // Idempotent WiFi radio init (STA mode, never connects — just enables the
    // radio for scanning). Pre-called in begin() when WiFi scanning is enabled
    // so the WiFi stack comes up before NimBLE (ARCHITECTURE.md coex ordering).
    void ensureWifi();
    bool _kickWifiScan();   // start one async passive sweep; returns true if it launched
    void _startWifi();
    void _stopWifi();
    void _pollWifi();       // consume completed sweep + re-kick; called from tick()

    // Phase sequencing helpers.
    void _startDomain(uint8_t domain);   // SCAN_BLE / SCAN_WIFI
    void _stopDomain(uint8_t domain);
    void _advancePhaseIfDue();           // BLE→WiFi handoff at the phase boundary
    void _applyScanSettingsChange();     // react to a live SMASK_SCAN change

    // Post EV_SCAN_STATE_CHANGED{domain, active}. Called on every radio
    // start/stop so the UI (and any listener) can track per-domain scan state.
    void _emitScanState(uint8_t domain, bool active);
};

extern ScanEngine g_scan_engine;
