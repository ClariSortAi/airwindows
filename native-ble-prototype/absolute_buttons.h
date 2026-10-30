#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>

// Absolute report3 has three button bits followed by little-endian unsigned XY.
// A global button action retains the last position; its first use centers the
// neutral pointer without synthesizing a primary click.
inline std::vector<uint8_t> AbsoluteButtons(const std::vector<uint8_t>& last, bool known, uint8_t buttons) {
    if(buttons>7)throw std::invalid_argument("Absolute mouse has three buttons");
    auto report=known?last:std::vector<uint8_t>{0,0,64,0,64};
    if(report.size()!=5 || report[2]>127 || report[4]>127)
        throw std::invalid_argument("Absolute pointer must have valid 15-bit XY");
    report[0]=buttons;
    return report;
}
