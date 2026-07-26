// PowerTree — Core layer: Clock seam.
// Owns "now" (injectable so recurrence/overdue logic is testable with a fake
// now) plus the ISO 8601 parse/format helpers (pure string <-> time_point).
//
// Real impls (SystemClock::now, parseIso8601, toIso8601) are TODO — defined in
// Clock.cpp during the Core build pass. FixedClock is a complete test double.

#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace powertree::core {

class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::system_clock::time_point now() const = 0;

    // Pure string <-> time_point math; no dependency on now().
    static std::optional<std::chrono::system_clock::time_point>
        parseIso8601(const std::string&);
    static std::string
        toIso8601(std::chrono::system_clock::time_point);
};

class SystemClock : public Clock {
public:
    std::chrono::system_clock::time_point now() const override;  // TODO: std::chrono::system_clock::now()
};

class FixedClock : public Clock {   // test double: a fixed "now"
    std::chrono::system_clock::time_point fixed_;
public:
    explicit FixedClock(std::chrono::system_clock::time_point t) : fixed_(t) {}
    std::chrono::system_clock::time_point now() const override { return fixed_; }
};

} // namespace powertree::core
