#pragma once

#include "event_ids.h"
#include "vibe_pattern.h"
#include <stddef.h>
#include <stdint.h>

constexpr size_t VIBE_MENU_OPTION_COUNT =
    VIBE_PATTERN_CATALOG_COUNT - 1;

enum class VibeMenuAction : uint8_t {
    NONE,
    PLAY_SELECTED,
    STOP,
};

struct VibeMenuDecision {
    bool consumed;
    VibeMenuAction action;
    HapticPatternId pattern;
};

const VibePatternInfo* vibeMenuPatternAt(size_t index);

class VibeMenuModel {
public:
    size_t selectedIndex() const { return _selected_index; }
    const VibePatternInfo* commit(size_t index);
    VibeMenuDecision handleButton(EventId event_id, bool repeating);

private:
    const VibePatternInfo* _cycle(bool next);

    size_t _selected_index = 0;
    bool _suppress_center_hold = false;
};
