# PowerTree — Class & Function Design (§11.1)

**Status:** Design-complete — all five layers locked (§11.1–§11.4 + nice-to-haves)
**Last updated:** 2026-07-26 (synced duration/reparenting/upcoming/next with HANDOFF; storageBoardId term fix)

---

## Locked Design Decisions

### 1. Layering

Four tiers, top to bottom:

```
domain/     Task, Board, enums (Status, RecurrenceRule)
core/       Clock, IdGenerator (injectable seams), Result/error types
repo/       Repository — JSON I/O, atomic writes, lock, journal, backups
services/   TaskService, BoardService — validation, state machine, recurrence, tree rules
cli/        CLI11 subcommand callbacks → handler functions → Formatter
main.cpp    Application context; owns the one-shot lifecycle
```

Two injectable seams from the hexagonal pattern: **Clock** (so recurrence/overdue logic is testable with a fake "now") and **IdGenerator** (so UUID generation is deterministic in tests).

---

### 2. Entity representation

- `Task` and `Board` are plain **`struct`s** (value types, all-public members).
- Relationships are stored as **ID strings only** — `ParentID`, `ChildIDs`, `BoardID`, `TaskIDs` — matching the JSON exactly.
- Repository holds `std::vector<Task>` + `std::unordered_map<std::string, size_t>` for O(1) ID resolution.
- `Status` and `RecurrenceRule` are **`enum class`**.
- **Strong-typedef wrappers** `TaskId` and `BoardId` — small wrapper structs with implicit `const std::string&` conversion, `operator==`, and `std::hash` specialization. Prevents passing a board ID where a task ID is expected at compile time. Boilerplate is ~15–20 lines per type, written once.
- **DateTime fields** (`createdAt`, `dueDate`) stored as **`std::string`** (ISO 8601) in the struct. Conversion to `time_point` happens only inside `Clock`/`TimeUtil` when arithmetic is needed (recurrence, overdue check).

---

### 3. Validation

- **§9 field rules** (Title length, Priority range, Tags format, DueDate, etc.) live in a **stateless `Validator` module** — free functions returning `Result`/error, individually unit-testable with no repo or disk.
- **State-machine and relational rules** (§4, §5) live inside the **Services**, which have repo context.
- **Deserialization is lenient:** load everything parseable; don't throw on a §9 violation. After hydration, run a validation pass and collect `ValidationWarning`s.
  - Warnings are printed to **stderr** on startup.
  - Any **mutating command is blocked** until the file is clean (`Repository::isDirty()` / `loadWarnings()`).
  - Parse errors (malformed JSON, missing primary key) still hard-error.

---

### 4. §5.4 Startup integrity routine (runs after lenient load)

Runs silently after hydration; auto-repairs structural inconsistencies:

| Check | Action |
|---|---|
| Task `BoardID` not in boards.json (orphan) | Overwrite with `storageBoardId` |
| Task and its parent have different `BoardID` | Overwrite task's `BoardID` with parent's |
| Subtree depth > 4 | Sever bidirectionally: clear task's `ParentID` → `""`, remove task from parent's `ChildIDs`, reassign task's `BoardID` → `storageBoardId` (promotes to root task) |
| Task has > 50 children | Collect as `ValidationWarning`; block writes (cannot auto-repair without dropping relationships) |

---

### 5. State machine

**Transition table** — a static `unordered_map<Status, unordered_set<Status>>`:

```
ToDo       → InProgress, Pending, Done, Cancelled
InProgress → ToDo, Pending, Done, Cancelled
Pending    → ToDo, InProgress, Done, Cancelled
Done       → ToDo  (reopen only)
Cancelled  → ToDo  (reopen only)
```

**Command-level gating** on top of the table:
- `reopen` is the **only** path that calls `transition(id, ToDo)` for Done/Cancelled tasks.
- `update --status` explicitly forbids `ToDo` as a target (can move to InProgress/Pending/Cancelled, never resurrect).

**Outside the table:**
- **Archive** — `IsArchived` is an orthogonal boolean flag; can be set/cleared on any task regardless of status.
- **Delete (discard)** — behavioral wipe, not a status. Precondition: `IsArchived == true OR Status == Done OR Status == Cancelled`. Triggers tree cleanup (§5.2/5.3).

---

### 6. Command dispatch

- **Handler functions** per command (`handleAdd`, `handleUpdate`, `handleComplete`, …).
- Wired via **CLI11 subcommand callbacks** — each subcommand's `->callback(...)` calls the matching handler.
- An **`Application` context** object (constructed in `main`) owns the one-shot lifecycle and holds Repository + Services + Clock + Formatter. Handlers receive it.

**One-shot lifecycle (§1.1):**
1. Sweep stray `*.tmp` files
2. Acquire process lock (`.todo/.lock`)
3. Hydrate JSON → in-memory structs
4. Run §5.4 integrity routine (silent auto-repairs)
5. Run §9 validation pass → collect `ValidationWarning`s; print to stderr if any
6. Dispatch to handler (block mutating commands if `isDirty()`)
7. Atomic persist (if mutation occurred)
8. Release lock & exit

---

### §11.2 Settings / Configuration System

**Storage:** `userdata.json` via the existing `Repository` userdata accessors. Full settings system exposed via `todo config get <key>`, `todo config set <key> <value>`, `todo config list`.

**`UserData` struct (typed for known CLI settings; `extra` map for future UI-owned prefs):**

```cpp
struct UserData {
    std::string activeBoardId;
    std::string storageBoardId;               // safety-net for orphan repair (§5.4); NOT "where new tasks go"
    std::string defaultSort              = "due";
    int         defaultPriority          = 5;
    int         defaultRecurrenceInterval = 1;
    std::string theme                    = "default";
    std::string dateFormat               = "YYYY-MM-DD";
    bool        confirmDiscard           = true;
    bool        showArchived             = false;   // ↔ --archived flag per-invocation
    bool        showDone                 = false;   // ↔ --show-done flag per-invocation
    bool        showCancelled            = false;   // ↔ --show-cancelled flag per-invocation
    int         upcomingCount            = 5;       // default N for `todo upcoming`
    std::string upcomingWindow           = "P3D";   // default window for `todo upcoming`
    std::unordered_map<std::string, std::string> extra;  // UI-owned prefs; CLI passes through untouched
};
```
---

### §11.3 Lock-file mechanics

`.todo/.lock` holds **PID + timestamp**. Acquired exclusively (`O_CREAT|O_EXCL`) before any read; deleted on exit.

| Parameter | Value |
|---|---|
| Retry cadence | 50ms |
| Acquire timeout | 2s |
| Stale age cap | 30s |
| Dead-PID | immediate reclaim |

**Stale detection — two independent signals:**
- Dead PID → reclaim immediately.
- Lock older than 30s → reclaim even if PID appears live (backstop against PID recycling).

**Reclamation:** delete `.lock`, re-acquire with `O_CREAT|O_EXCL` (prevents two simultaneous reclaimers both winning).

**`ProcessChecker` seam** (consistent with `Clock`/`IdGenerator`):

```cpp
class ProcessChecker {
public:
    virtual ~ProcessChecker() = default;
    virtual bool isAlive(long pid) const = 0;
};
class SystemProcessChecker : public ProcessChecker { ... };  // Windows: OpenProcess
class FakeProcessChecker   : public ProcessChecker { ... };  // test: scripted alive/dead
```

`LockFile` takes a `ProcessChecker&`. Makes reclamation logic unit-testable and cross-platform.

---
### §11.4 Exit-code finalization

Finalized scheme (mirrors §10.1 of the Master Spec):

| Code | Meaning | Triggers |
|---|---|---|
| `0` | Success | Normal completion; read-only commands succeed even on dirty data |
| `1` | Validation / input / state error | §9 field failure, state-machine violation, not-found, dirty-data blocks mutation |
| `2` | System / I/O / lock error | JSON parse failure, lock timeout, fsync/rename failure, backup I/O failure |
| `3` | User aborted | User answered `n` to a confirmation prompt (fuzzy-match confirm, `confirmDiscard`) |

**Why `3`:** the future ImGui frontend must distinguish "command failed on bad input" (show error) from "user declined a confirmation" (do nothing, silent).

**`ErrorCode` → exit code:** `Validation`/`NotFound`/`StateConflict`/`Dirty` → `1`; `IoError`/`LockFailure` → `2`; user-abort signalled at CLI/Formatter layer → `3`.
---


### Layer 1 — Domain (pure data, no dependencies)

```cpp
enum class Status { ToDo, InProgress, Pending, Done, Cancelled };
enum class RecurrenceRule { None, Hourly, Daily, Weekly, Monthly, Yearly };

struct RecurrenceConfig {
    RecurrenceRule rule = RecurrenceRule::None;
    int interval = 1;              // e.g. 2 = every 2 weeks; room to extend later
};

struct TaskId  { /* strong-typedef wrapper over std::string */ };
struct BoardId { /* strong-typedef wrapper over std::string */ };

// Reminder — shared optional value type; works on ANY task (event-like or not)
struct Reminder {
    std::string offsetOrTime;  // ISO 8601 duration ("PT15M" = 15m before dueDate) OR absolute datetime
    bool        fired = false; // set by fire-layer once surfaced; data-only in CLI
};

struct Task {
    std::string id;                // full UUIDv4
    std::string title;             // 1–255
    std::string description;       // ≤8192
    int priority = 5;              // 0–10
    Status status = Status::ToDo;
    RecurrenceConfig recurrence;
    std::string parentId;          // "" if root
    std::vector<std::string> childIds;   // ≤50
    std::string boardId;
    std::string createdAt;         // ISO 8601 string
    std::string dueDate;           // ISO 8601, "" if none. Time component present => event start
    std::optional<std::string> duration; // ISO 8601 duration (e.g. "PT1H"); presence => event-like. Computed end = dueDate + duration (never stored absolute)
    std::vector<Reminder> reminders;     // empty = none
    bool isArchived = false;
    std::vector<std::string> tags; // ≤25
};

struct Board {
    std::string id;
    std::string name;              // 1–100, unique
    std::string description;
    std::vector<std::string> taskIds;
};
```
---

### Layer 2 — Core seams (std lib only)

**Toolchain confirmed:** GCC 13.4.0, MinGW-w64, CMake 4.0.2 → **C++23**. Use `std::expected<T, Error>` (no hand-rolled Result). `std::format` available (C++20). Set in CMakeLists.txt: `set(CMAKE_CXX_STANDARD 23)`, `set(CMAKE_CXX_STANDARD_REQUIRED ON)`, `set(CMAKE_CXX_EXTENSIONS OFF)`.

```cpp
// Error handling — flows through Services to CLI boundary
enum class ErrorCode { Validation, NotFound, StateConflict, IoError, LockFailure, Dirty };
struct Error { ErrorCode code; std::string message; };
// Services return std::expected<T, Error>; CLI maps ErrorCode -> exit code (§10.1)

// Clock seam — owns "now" AND ISO 8601 parse/format (TimeUtil folded in)
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::system_clock::time_point now() const = 0;
    static std::optional<std::chrono::system_clock::time_point> parseIso8601(const std::string&);
    static std::string toIso8601(std::chrono::system_clock::time_point);
};
class SystemClock : public Clock { ... };   // real
class FixedClock  : public Clock { ... };   // test double (fixed now)

// IdGenerator seam — produces raw UUIDv4 only; collision-retry lives in Repository
class IdGenerator {
public:
    virtual ~IdGenerator() = default;
    virtual std::string generate() const = 0;
};
class UuidGenerator       : public IdGenerator { ... };  // real
class SequentialIdGenerator : public IdGenerator { ... }; // test: predictable sequence
```

**Notes:**
- Collision check (generate → test 6-char prefix → regenerate) is a **Repository** concern; generator stays dumb. Tested by injecting a sequence that forces a prefix collision.
- `parseIso8601` returns `optional` so the Validator can use it to check DueDate.

---

### Layer 3 — Repository (data gateway)

**Design:** `Repository` facade over four focused components. Services see one object; each mechanism is independently testable.

| Class | Responsibility |
|---|---|
| `JsonStore` | Raw load/save of one JSON file + atomic write (temp/fsync/rename) + `.tmp` sweep |
| `LockFile` | Acquire/release `.todo/.lock`; stale-lock reclamation (PID+timestamp check) |
| `Journal` | Snapshot push/pop; cap 20; atomic stack-file writes |
| `BackupManager` | 24h trigger check; plain-file copy to timestamped folder; rotate keep-7 |
| `Repository` | In-memory `Task`/`Board` collections + prefix maps; CRUD methods; orchestrates the above; exposes `isDirty()` / `loadWarnings()` |

**Lifecycle note:** `Repository` exposes operations (`load()`, `commit()`, `acquireLock()`, `snapshot()`). The `Application` context (Layer 5) sequences them — Repository does not know about command dispatch.

**Collision-retry** (§6.1) lives in `Repository::generateId()`: calls `IdGenerator::generate()`, checks the in-memory prefix set, retries on collision.

---


**Key clarifications:**
- `activeBoardId` — set via `todo board switch`; new tasks land here when `-b` is not specified.
- `storageBoardId` — used only by §5.4 integrity repair (orphan tasks, depth-severed tasks). Not a normal workflow destination.
- `showArchived`/`showDone`/`showCancelled` — default list filter settings; per-invocation flags override them. Default `todo list` shows only ToDo/InProgress/Pending non-archived tasks.
- `extra` map — unvalidated by CLI; reserved for UI preferences added without a schema change.

**CLI surface:**
```
todo config get <key>
todo config set <key> <value>
todo config list
```
Known keys are validated against the typed struct fields; unknown keys go into `extra`.

---

### Calendar / Events / Reminders

**Event = Task, not a separate entity.** A task is "event-like" when `dueDate` carries a time component (start) and/or `duration` is set. Keeps boards, tags, subtasks, recurrence, state machine, sorting for free. A "calendar" is a view/filter over tasks with datetimes, not a separate store.

**Precision** — inferred from the ISO 8601 string itself (RFC 5545 style): `2026-07-25` = date-only, `2026-07-25T15:00:00+07:00` = datetime. No parallel precision field. Only the derived **overdue** check branches on it: date-only overdue at end-of-day, datetime overdue at the instant.

**New fields on `Task`:**
- `std::optional<std::string> duration` — ISO 8601 duration (e.g. `PT1H`); presence makes the task event-like. The end time is **computed** (`dueDate + duration`), never stored as an absolute `endTime`. As a result, editing `dueDate` automatically carries the end time with it — no second field to keep in sync.
- `std::vector<Reminder> reminders` — shared optional; works on any task (deadline reminder or event reminder).

**`Reminder`** — `{ std::string offsetOrTime; bool fired = false; }`. `offsetOrTime` is either an ISO 8601 duration offset (`-PT30M` = 30 min before `dueDate`) or an absolute ISO 8601 datetime.

**Reminder recalculation on `dueDate` edit** — on `update --due`, `TaskService::update` recomputes every duration-offset reminder's fire time and resets `fired = false` **unconditionally**. If the new fire time is already in the past, the next `due-reminders` run fires it immediately; deciding that at edit time would be more code for no gain. Absolute-datetime reminders are unaffected by a dueDate change.

**New validation rules:**
- `validation::duration` — must be a valid **positive** ISO 8601 duration, **days/hours/minutes/seconds only**. Reject `P[n]Y` and `P[n]M` (months) with a clear "use days instead (e.g. `P30D`)" error.
- `validation::reminder` — must be a valid ISO 8601 duration OR absolute datetime.
- Relational (in `TaskService`): `dueDate` must have a time component when `duration` is set; reminder fire time before `dueDate` and (on create) in the future. (The old `endTime > dueDate` relational check is dropped — with a positive duration the computed end is always after the start by construction.)

**ISO 8601 duration parser** — hand-rolled, no new dependency (~50 lines). Supports `P[n]DT[n]H[n]M[n]S` combinations. The `T` separator is the only real footgun: it disambiguates minutes (`PT1M`) from months (`P1M`) — the parser tracks whether `T` has been seen. Years/months are rejected at parse time (see `validation::duration` above). Exposed as a stateless `duration` namespace alongside `recurrence` (see Layer 4).

**New CLI commands (fire-layer / daily use):**
- `todo next` — first task by `dueDate` ascending. Tasks with no `dueDate` are ineligible; if none qualify, print a short warning to stderr and exit 0. Read-only.
- `todo upcoming [--n <count>] [--within <duration>]` — next N due/event tasks by start time, optionally within a window. Read-only. Defaults `upcomingCount = 5` and `upcomingWindow = "P3D"` come from `UserData` (settable via `todo config set`); `--n` / `--within` override per-invocation.
- `todo show <id>` — detailed single-task view (all fields + children). Resolver + Formatter already exist. Read-only.
- `todo tree [id]` — ASCII subtask hierarchy walk. Formatter + existing parent/child model. The walk keeps a `std::unordered_set<std::string> visited` cycle guard as a safety net against hand-edited/corrupted JSON that validation never saw. Read-only.
- `todo due-reminders` — emits reminders whose fire time has passed and `fired == false`, then marks them `fired = true`. This is the command an external scheduler (Task Scheduler / cron) or the future GUI polls; it surfaces "these are due" and marks them consumed. **The CLI never fires notifications itself.**

**`due-reminders` — exempt from journal (non-journaled mutation):**

The Application classifies commands into three buckets:

| Class | snapshot? | commit? | examples |
|---|---|---|---|
| Read-only | no | no | `list`, `upcoming`, `show`, `next`, `tree` |
| Journaled mutation | yes | yes | `add`, `update`, `complete`, `discard` |
| Non-journaled mutation | no | yes | `due-reminders` |

`due-reminders` commits `fired=true` to disk but skips `repo.snapshot()`, so the undo journal never sees it. `undo` only rewinds real user actions. A fired reminder is intentionally not undoable. (Double-fire on a crash between "compute which to fire" and "write `fired=true`" is accepted for a CLI tool — no snapshot/transaction spans that gap.)

**`todo list --tag <tag>`** — filter by tag (one optional field on `ListFilter`).

**Relative date input (CLI layer — locked):** `dueDate` (and other date inputs) accept `tomorrow`, `+3d`, `+2w`, etc.; the CLI layer expands these to ISO 8601 before the value reaches the service. Keeps date math out of the service layer; biggest quality-of-life win for daily use.

### Re-parenting (now supported)

`UpdateTaskParams` gains an optional `parentId`; re-parenting is supported through the normal update path (previously add-time only).

- **Sentinel:** `parentId == ""` = top-level/root. Any empty/absent value normalizes to `""`.
- **Subtree moves with the task** — it's a move, not a flatten; children stay attached.
- **Re-parent checks (in `TaskService::checkReparent`, in order):**
  1. Normalize empty → `""` (root); if root, skip the remaining checks.
  2. `newParentId != taskId` — no self-parenting.
  3. `newParentId` must reference an existing task.
  4. `newParentId` must NOT be in the task's descendant set — O(subtree) downward walk collecting descendants. Prevents cycles at the source.
- Still honor subtask depth ≤4 and ChildIDs ≤50 on the destination after a move.

---

### Layer 4 — Services (business logic)

- **`validation` namespace** — stateless free functions for §9 field rules (`title`, `description`, `priority`, `tags`, `boardName`, `dueDate`, `duration`, `reminder`). `dueDate` takes a `Clock` so "not past" is testable. The namespace stays **purely stateless** — no repo, no exceptions. Board-name *uniqueness*, the `reminder` fire-time rule, and the **re-parent checks** are all relational → they live in the services that hold repo context (`BoardService`, `TaskService::checkReparent` — see "Re-parenting" above).
- **`recurrence` namespace** — pure §4.3 date math (`nextDueDate`), free functions taking `RecurrenceConfig` + anchor + `Clock`. Unit-testable without a service.
- **`duration` namespace** — pure, stateless ISO 8601 duration parsing + application. `parse(string) → expected<Parsed,Error>` (a small `{days, hours, minutes, seconds}` struct) and `apply(time_point, Parsed) → time_point`. Supports `P[n]DT[n]H[n]M[n]S`; rejects years/months. The `T`-separator state disambiguates minutes from months. No new dependency.
- **Param structs** — `AddTaskParams` (optionals + `priority=5` default; exposes `duration` + `reminders` so an event-like task and its reminders can be set at creation), `UpdateTaskParams` (all `std::optional`, `ToDo` status forbidden, **gains optional `parentId`** for re-parenting, plus `duration`/`reminders`), `ListFilter` (gains optional `tag` for `todo list --tag`).
- **`Resolver`** (§8) — produces candidates (exact prefix → Levenshtein); returns `Exact` / `Ambiguous` / `None`. The **confirm/pick prompting is a CLI concern**; services receive an already-resolved `TaskId`/`BoardId`.
- **`TaskService`** — holds `Repository&` + `Clock&`; methods: `add`, `update` (recalc duration-offset reminders + reset `fired` on any `--due` change; run `checkReparent` on any `--parent` change), `complete` (recurrence clone), `reopen`, `cancel`, `postpone`, `archive`, `discard`, `undo`, `list`, `next` (first by `dueDate` asc; no-due ineligible; returns `std::optional<Task>` — `nullopt` = none qualify, handler warns + exit 0, not an error), `upcoming` (N tasks within a window), `tree` (pre-order subtask walk with `visited` cycle guard). Private `transition(Task&, Status)` consults the static transition table; private `checkReparent` runs the re-parent cycle checks.
- **`BoardService`** — `add`, `switchTo`, `remove` (blocked if non-empty, §5.1), `list`.


---

### Layer 5 — CLI / Application

- **`Formatter`** — single class with `Format` enum (`Text`/`Json`). Owns all output: `print(Task)`, `print(vector<Task>)`, `print(Board)`, `print(vector<Board>)`, `printWarnings`, `printError`. Also owns Resolver interaction: `confirm("Did you mean X? [y/N]")` and `pick(candidates)` — prompts to stderr, reads stdin. Future UI captures stderr separately (§1.2 / §10.2).
- **Recurrence CLI flags** — `-r <rule>` accepts `hourly|daily|weekly|monthly|yearly`; `--interval <n>` (default 1). Populates `RecurrenceConfig{rule, interval}`. Extensible for future ruleset additions.
- **Handler functions** — one free function per command. Each: resolves `<ID>` via `Resolver` + `Formatter::confirm/pick`, builds param struct, calls service, formats result, returns exit code.
- **CLI11 wiring** — subcommand definitions with flags in `Application::run()`; each `->callback(...)` fires the matching handler.
- **`Application`** — owns the one-shot lifecycle and all collaborators (`Repository`, `TaskService`, `BoardService`, `Resolver`, `Formatter`). `run(int argc, char** argv)` returns exit code.

**Lifecycle order (§1.1):**
1. `JsonStore::sweepTmp()`
2. `repo_.acquireLock()`
3. `repo_.load()`
4. `repo_.runIntegrity()` — silent auto-repairs
5. Print `loadWarnings()` to stderr if any
6. Parse args (CLI11); block mutating commands if `repo_.isDirty()`
7. `repo_.snapshot()` — mutating commands only
8. Dispatch handler
9. `repo_.commit()` — mutating commands only
10. `repo_.release()`

**ErrorCode → exit code (§11.4):** `Validation`/`NotFound`/`StateConflict`/`Dirty` → 1; `IoError`/`LockFailure` → 2; user-abort (answered `n` to a confirm/pick prompt, or `confirmDiscard`) is signalled at the CLI/Formatter layer → 3. See the full table in §11.4.

---