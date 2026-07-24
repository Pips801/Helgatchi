#pragma once
#include <stdint.h>
#include "event_payload.h"

// Implement this interface in any service that subscribes to events.
class IEventHandler {
public:
    virtual void onEvent(const Event& e) = 0;
protected:
    ~IEventHandler() = default;
};

class EventBus {
public:
    static constexpr uint8_t  QUEUE_DEPTH       = 64;
    static constexpr uint8_t  MAX_SUBSCRIPTIONS = 128;

    // Call once during setup(), after FreeRTOS scheduler is running.
    void begin();

    // Register handler for a specific event/command ID.
    // Returns false if MAX_SUBSCRIPTIONS exceeded.
    // Subscribe only during setup() — not thread-safe.
    bool subscribe(EventId id, IEventHandler* handler);

    // Register a catch-all handler that receives every event/command.
    // Uses one subscription slot. Ideal for loggers and monitors.
    bool subscribeAll(IEventHandler* handler);

    // Post an event onto the queue. Thread-safe; safe to call from any task.
    // Returns false and increments dropped counter if queue is full.
    bool post(EventId id);
    bool post(EventId id, const EventPayload& payload);

    // Drain the current queue contents and dispatch to subscribers.
    // Call from the main loop task. Handlers may post new events;
    // those will be dispatched on the next dispatch() call.
    void dispatch();

    uint32_t droppedCount() const { return _dropped; }

    // Lifetime count of events dispatched since boot (diagnostics). Unlike the
    // perf counters below, this is never reset.
    uint32_t eventCount() const { return _total_events; }

    // Performance counters — updated inside dispatch(). LogService reads
    // and resets these once per second when at DEBUG_RENDERING_PERF.
    struct PerfStats {
        uint32_t events;       // events handled this window
        uint32_t dispatches;   // dispatch() calls this window
        uint32_t total_us;     // sum of dispatch() wall-clock time
        uint32_t max_us;       // longest dispatch() in this window
    };
    PerfStats perfSnapshotAndReset();

private:
    struct Subscription {
        EventId       id;
        IEventHandler* handler;
    };

    Event        _queue[QUEUE_DEPTH];
    uint8_t      _head      = 0;
    uint8_t      _tail      = 0;
    uint8_t      _count     = 0;

    Subscription _subs[MAX_SUBSCRIPTIONS];
    uint8_t      _sub_count = 0;

    uint32_t     _dropped      = 0;
    uint32_t     _total_events = 0;   // lifetime dispatched count (never reset)

    // Perf accumulators (reset each second by LogService).
    uint32_t     _perf_events     = 0;
    uint32_t     _perf_dispatches = 0;
    uint32_t     _perf_total_us   = 0;
    uint32_t     _perf_max_us     = 0;

    void*        _mutex     = nullptr; // SemaphoreHandle_t, opaque to avoid FreeRTOS header cascade
};

extern EventBus g_bus;
