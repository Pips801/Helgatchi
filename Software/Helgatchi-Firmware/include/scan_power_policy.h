#pragma once

#include <stdint.h>

namespace scan_power_policy {

constexpr uint8_t enabledRadioCount(uint32_t mode) {
    return static_cast<uint8_t>(((mode & 1u) ? 1u : 0u) +
                                ((mode & 2u) ? 1u : 0u));
}

constexpr uint32_t windowMs(uint32_t mode, uint16_t duration_s) {
    return static_cast<uint32_t>(duration_s) * 1000u *
           enabledRadioCount(mode);
}

constexpr uint64_t wakeIntervalUs(uint32_t mode, uint16_t sleep_s) {
    return enabledRadioCount(mode) == 0
        ? 0ULL
        : static_cast<uint64_t>(sleep_s) * 1000000ULL;
}

}  // namespace scan_power_policy
