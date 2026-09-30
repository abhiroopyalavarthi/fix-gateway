#pragma once

// Decimal price strings from the wire <-> integer ticks (cents).
// Parsed by hand instead of through double: "190.25" has to become exactly
// 19025, and "190.255" has to be rejected, not rounded.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace price {

constexpr int kDecimals = 2;  // one tick = $0.01

// Returns ticks, or nullopt if the text isn't a positive decimal on the tick grid.
// Trailing zeros past the grid are fine ("190.2500"), other digits aren't.
inline std::optional<int64_t> parse(std::string_view s) {
    if (s.empty() || s.size() > 18) return std::nullopt;
    int64_t whole = 0;
    size_t i = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        whole = whole * 10 + (s[i] - '0');
        ++i;
        any = true;
    }
    int64_t frac = 0;
    int digits = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            int d = s[i] - '0';
            if (digits < kDecimals) {
                frac = frac * 10 + d;
            } else if (d != 0) {
                return std::nullopt;  // off the tick grid
            }
            ++digits;
            ++i;
            any = true;
        }
    }
    if (!any || i != s.size()) return std::nullopt;
    for (int k = digits; k < kDecimals; ++k) frac *= 10;
    int64_t ticks = whole * 100 + frac;
    if (ticks <= 0) return std::nullopt;
    return ticks;
}

inline std::string format(int64_t ticks) {
    std::string out = std::to_string(ticks / 100) + ".";
    int64_t c = ticks % 100;
    if (c < 10) out += '0';
    out += std::to_string(c);
    return out;
}

}  // namespace price
