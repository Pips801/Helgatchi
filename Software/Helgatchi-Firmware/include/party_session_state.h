#pragma once

#include "event_ids.h"
#include <stdint.h>

enum class PartySessionMode : uint8_t {
    INACTIVE,
    TIMED,
    MENU,
};

class PartySessionState {
public:
    void startTimed(uint32_t now_ms, uint32_t duration_ms);
    void startMenu();
    bool stop();
    bool consumeMenuExitButton(EventId event_id);

    bool expired(uint32_t now_ms) const;
    uint32_t remainingMs(uint32_t now_ms) const;

    bool active() const { return _mode != PartySessionMode::INACTIVE; }
    bool timed() const { return _mode == PartySessionMode::TIMED; }
    bool menu() const { return _mode == PartySessionMode::MENU; }
    PartySessionMode mode() const { return _mode; }

private:
    void _clear();

    PartySessionMode _mode = PartySessionMode::INACTIVE;
    uint32_t _started_ms = 0;
    uint32_t _duration_ms = 0;
};
