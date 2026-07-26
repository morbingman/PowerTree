// PowerTree — Domain layer: core entity value types (Task, Board).
// Plain structs, all-public members, linked by ID strings (matching the JSON
// layout exactly). No behaviour, no dependencies beyond the std library.
// DateTime fields are ISO 8601 strings; arithmetic happens only inside Clock.

#pragma once

#include <string>
#include <vector>
#include <optional>

namespace powertree::domain {

// Task lifecycle states. Overdue and Recurring are derived (not stored).
enum class Status { ToDo, InProgress, Pending, Done, Cancelled };

// Recurrence cadence. "None" = a one-off task.
enum class RecurrenceRule { None, Hourly, Daily, Weekly, Monthly, Yearly };

struct RecurrenceConfig {
    RecurrenceRule rule     = RecurrenceRule::None;
    int            interval = 1;   // e.g. 2 = every 2 weeks; extensible later
};

// A reminder works on any task. fire time is derived from offsetOrTime:
//   - an ISO 8601 duration offset (e.g. "-PT30M" = 30 min before dueDate), or
//   - an absolute ISO 8601 datetime.
// `fired` is set once the reminder is surfaced (by the fire-layer); data-only
// in the CLI, which never fires notifications itself.
struct Reminder {
    std::string offsetOrTime;
    bool        fired = false;
};

// A Task is the unit of work. It is "event-like" when dueDate carries a time
// component (the start) and/or duration is set (end = dueDate + duration,
// computed, never stored). "Event" is not a separate entity.
struct Task {
    std::string                id;            // full UUIDv4
    std::string                title;         // 1-255
    std::string                description;   // <= 8192
    int                        priority    = 5;  // 0-10; 0=emergency, 10=non-issue
    Status                     status      = Status::ToDo;
    RecurrenceConfig           recurrence;
    std::string                parentId;      // "" if root
    std::vector<std::string>   childIds;      // <= 50; subtree depth <= 4
    std::string                boardId;
    std::string                createdAt;     // ISO 8601, immutable
    std::string                dueDate;       // ISO 8601, "" if none; time component => event start
    std::optional<std::string> duration;      // ISO 8601 duration (e.g. "PT1H"); presence => event-like
    std::vector<Reminder>      reminders;     // empty = none
    bool                       isArchived  = false;
    std::vector<std::string>   tags;          // <= 25, each 1-50, [a-z0-9-_]
};

// A Board is the structural ledger grouping tasks. Board name is unique.
struct Board {
    std::string              id;
    std::string              name;          // 1-100, unique
    std::string              description;
    std::vector<std::string> taskIds;       // enables O(1) empty checks
};

} // namespace powertree::domain
