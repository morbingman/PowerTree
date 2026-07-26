// PowerTree — Core layer: ProcessChecker seam.
// Used by LockFile to decide whether a lock-holding PID is still alive (stale
// lock reclamation — §6.4). Injectable so reclamation logic is unit-testable
// and cross-platform.
//
// SystemProcessChecker (Windows OpenProcess / POSIX kill(pid,0)) is TODO —
// defined in ProcessChecker.cpp during the Core build pass. FakeProcessChecker
// is a complete test double.

#pragma once

#include <functional>

namespace powertree::core {

class ProcessChecker {
public:
    virtual ~ProcessChecker() = default;
    virtual bool isAlive(long pid) const = 0;
};

class SystemProcessChecker : public ProcessChecker {
public:
    bool isAlive(long pid) const override;   // TODO: Windows OpenProcess / POSIX kill(pid,0)
};

class FakeProcessChecker : public ProcessChecker {  // test double: scripted alive/dead
    std::function<bool(long)> behavior_;
public:
    explicit FakeProcessChecker(std::function<bool(long)> b) : behavior_(std::move(b)) {}
    bool isAlive(long pid) const override { return behavior_ ? behavior_(pid) : false; }
};

} // namespace powertree::core
