// PowerTree — Domain layer: strong-typedef ID wrappers.
// Prevents passing a BoardId where a TaskId is expected at compile time.
// Pure data; std-lib only.

#pragma once

#include <string>        // std::string
#include <functional>    // std::hash

namespace powertree::domain {

// Strong-typedef wrapper over a UUIDv4 string. Implicit conversion to
// const std::string& for ergonomic use with existing string-based APIs;
// explicit construction prevents accidental empty/ambiguous ids.
struct TaskId {
    std::string value;

    explicit TaskId(std::string v) : value(std::move(v)) {}
    operator const std::string&() const { return value; }
    bool operator==(const TaskId&) const = default;
};

struct BoardId {
    std::string value;

    explicit BoardId(std::string v) : value(std::move(v)) {}
    operator const std::string&() const { return value; }
    bool operator==(const BoardId&) const = default;
};

} // namespace powertree::domain

// std::hash specializations so TaskId/BoardId can key unordered containers.
template <>
struct std::hash<powertree::domain::TaskId> {
    std::size_t operator()(const powertree::domain::TaskId& t) const noexcept {
        return std::hash<std::string>{}(t.value);
    }
};

template <>
struct std::hash<powertree::domain::BoardId> {
    std::size_t operator()(const powertree::domain::BoardId& b) const noexcept {
        return std::hash<std::string>{}(b.value);
    }
};
