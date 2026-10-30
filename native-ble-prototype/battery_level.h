#pragma once
#include <cstdint>
#include <optional>

// SYSTEM_POWER_STATUS uses 255 for unknown. BAS Battery Level allows 0..100.
// BAS 1.1 section 3.1.1 specifies zero when no battery is present.
inline std::optional<uint8_t> BatteryLevel(bool available, uint8_t flags, uint8_t percent) {
    if (!available || flags == 255) return std::nullopt;
    if ((flags & 128) != 0) return uint8_t{0};
    if (percent > 100) return std::nullopt;
    return percent;
}
