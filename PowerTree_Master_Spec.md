# PowerTree — Master Design Specification

**C++ CLI To-Do Application**
Consolidated Architectural, Functional & Data Specification
Version 1.0 (Draft) · Consolidated 2026-07-25 · Updated 2026-07-26

---

## Table of Contents

- [0. Preface — Purpose & Provenance](#0-preface--purpose--provenance)
- [1. System Architecture & Execution Model](#1-system-architecture--execution-model)
- [2. File System & Storage Configuration](#2-file-system--storage-configuration)
- [3. Entity Data Models](#3-entity-data-models)
- [4. Lifecycle & State Machine Rules](#4-lifecycle--state-machine-rules)
- [5. System Integrity & Automated Failsafes](#5-system-integrity--automated-failsafes)
- [6. Identity, Safe Persistence & Data Safety](#6-identity-safe-persistence--data-safety)
- [7. Disaster Recovery (Rolling Backups)](#7-disaster-recovery-rolling-backups)
- [8. Fuzzy Matching & Typo Tolerance](#8-fuzzy-matching--typo-tolerance)
- [9. Validation Rules](#9-validation-rules)
- [10. CLI Specification](#10-cli-specification)
- [11. Resolved Design Decisions](#11-resolved-design-decisions)
- [12. Build & Dependency Toolchain](#12-build--dependency-toolchain)

---

## 0. Preface — Purpose & Provenance

This document is the single source of truth for the PowerTree application. It consolidates four prior source documents into one master specification and records the design decisions reached during review.

### Consolidated sources

- **todo_app_requirements.txt** — original high-level feature list.
- **Todo_App_Advanced_Features.txt** — tooling and stretch strategy (vcpkg, fuzzy search, backups).
- **Todo_App_SRS.docx** — detailed software requirements specification (the most authoritative prior source).
- **README.md** — currently empty.

### Reconciliation policy

Where the sources conflict, the SRS is authoritative, but useful ideas the SRS omitted have been folded back in (most notably a priority system). Superseded choices are recorded so intent is never lost.

> **Resolved conflicts:** Persistence is JSON only (CSV dropped). A 0–10 priority field is re-introduced. "Overdue" and "Recurring" become derived display states rather than persisted statuses.

> **Resolved items:** The sections formerly marked TBD — class/function design (§11.1), the settings system (§11.2), lock-file mechanics (§11.3), and final exit codes (§11.4) — were designed collaboratively and are now locked. See §11 for the resolution summary; the full class/function design lives in `PowerTree_Class_Design.md` with code signatures in `PowerTree_Sketches.h`.

---

## 1. System Architecture & Execution Model

PowerTree operates strictly as a decoupled C++ Command Line Interface (CLI) backend. It uses a "one-shot" execution model: each invocation is a fresh, short-lived process that loads state, performs one operation, persists, and exits.

### 1.1 One-shot execution lifecycle

1. **Invocation** — the OS launches the executable with arguments.
2. **Startup housekeeping** — sweep any stray `*.tmp` files (evidence of a previously interrupted write) and run the integrity routine (§5).
3. **Lock acquisition** — acquire the exclusive process lock before any read (§6.4).
4. **Hydration** — load and parse the relational JSON files into in-memory C++ structures (`std::vector` / hash maps).
5. **Execution** — validate inputs against the state machine and execute the business logic.
6. **Serialization** — write changes back atomically using UTF-8 encoding (§6.2).
7. **Lock release & termination** — release the lock, flush streams, release memory to the OS.

### 1.2 UI integration strategy

A future ImGui frontend will act as a persistent parent process. It will execute the CLI backend via sub-process calls, using explicit flags (e.g. `--format json`) to parse responses from standard output (stdout) and intercepting errors from standard error (stderr).

> **Design consequence:** Because a GUI parent may fire commands while another invocation is still running, concurrent invocations are realistic. This is the direct motivation for the process lock in §6.4.

---

## 2. File System & Storage Configuration

Data is persisted via standard JSON serialization inside a hidden `.todo` directory. By default this resides in the user's home path, but the location is overridable (§6.5).

### 2.1 Data directory layout

| Path | Purpose |
|---|---|
| `.todo/tasks.json` | Array of Task objects. Highly volatile; frequent read/writes. |
| `.todo/boards.json` | Array of Board objects; the structural ledger. |
| `.todo/userdata.json` | Typed `UserData` settings struct — active/storage board, defaults, display prefs, upcoming defaults, and an `extra` map (§11.2). |
| `.todo/journal.log` | Undo snapshot stack (§6.3). Capped at the last 20 operations. |
| `.todo/backups/` | Disaster-recovery copies, timestamped (§7). |
| `.todo/.lock` | Process lock file preventing concurrent writes (§6.4). |

### 2.2 Encoding & format rules

- All files are UTF-8 encoded.
- All DateTime fields use ISO 8601 strings (`YYYY-MM-DDTHH:MM:SSZ`) for cross-platform consistency.
- Duration fields use ISO 8601 duration form (e.g. `PT1H`, `P1DT30M`); only days/hours/minutes/seconds are supported — months and years are rejected (use days, e.g. `P30D`).
- Writes are atomic (temp file + fsync + rename); the live file is never observed half-written (§6.2).

---

## 3. Entity Data Models

All DateTime fields strictly adhere to ISO 8601. Primary keys are UUIDv4 strings whose first 6 characters are guaranteed unique at creation and serve as the user-facing handle (§6.1).

### 3.1 Task entity

| Field | Type | Description / Constraints |
|---|---|---|
| ID | String (UUIDv4) | First 6 chars guaranteed unique at creation; user-facing handle. Primary key. |
| Title | String | Mandatory headline. 1–255 chars. |
| Description | String | Optional. Max 8,192 chars. Supports Markdown / subtask parsing symbols. |
| Priority | Integer | 0–10. 0 = emergency, 10 = non-issue. Default 5. (Re-introduced from requirements.) |
| Status | Enum | ToDo, InProgress, Pending, Done, Cancelled. (Overdue/Recurring are derived, not stored.) |
| RecurrenceRule | String | None, Hourly, Daily, Weekly, Monthly, Yearly (+ optional interval config). |
| ParentID | String | UUID of parent. Empty string if root task. Settable at add time and via `update --parent` (re-parenting — §4.7). |
| ChildIDs | Array\<String\> | Direct subtask UUIDs. Max 50; subtree depth max 4. |
| CreatedAt | DateTime | Creation timestamp (ISO 8601). Immutable. |
| DueDate | DateTime | Optional deadline. Past dates blocked on manual entry. A time component makes the task "event-like" (the start); when Duration is set, DueDate must carry a time component. |
| Duration | String (ISO 8601 duration) | Optional. e.g. `PT1H`. Presence makes the task event-like; the end time is computed as DueDate + Duration and is never stored. Days/hours/minutes/seconds only (no months/years). |
| Reminders | Array\<Reminder\> | Optional. Each Reminder = `{ OffsetOrTime, Fired }` — OffsetOrTime is an ISO 8601 duration offset (e.g. `-PT30M`, relative to DueDate) or an absolute datetime; Fired is a boolean set once the reminder is surfaced. Empty = none. See §3.4. |
| IsArchived | Boolean | True = hidden from default views. False by default. |
| Tags | Array\<String\> | Max 25. Each 1–50 chars, lowercased, `[a-z0-9-_]`. |
| BoardID | String | Foreign key into boards.json. |

### 3.2 Board entity

| Field | Type | Description / Constraints |
|---|---|---|
| ID | String (UUIDv4) | Unique identifier. Primary key. |
| Name | String | 1–100 chars. Must be unique (prevents name-resolution conflicts). |
| TaskIDs | Array\<String\> | Ledger of task UUIDs on this board. Enables O(1) empty checks. |
| Description | String | Optional context regarding the board's purpose. |

### 3.3 Derived (non-persisted) states

These are computed at read/display time and never written to disk, avoiding needless rewrites on every startup:

- **Overdue** — true when DueDate < now and Status is not Done/Cancelled.
- **Recurring** — true when RecurrenceRule ≠ None.

### 3.4 Events & reminders

A "calendar" is not a separate store; it is a view/filter over tasks that carry datetimes. A task is **event-like** when its DueDate has a time component (the start) and/or a Duration is set (the end = DueDate + Duration). Event-like tasks reuse boards, tags, subtasks, recurrence, the state machine, and sorting for free.

**Precision** is inferred from the ISO 8601 string: `2026-07-25` is date-only (overdue at end-of-day), `2026-07-25T15:00:00+07:00` is a datetime (overdue at the instant). No separate precision field is stored.

**Reminders** work on any task. A reminder's fire time is either an absolute datetime or a duration offset relative to DueDate (e.g. `-PT30M` = 30 minutes before due). The fire time must be before DueDate and, at creation, in the future. The CLI never fires notifications itself; `todo due-reminders` (§10.5) is the command an external scheduler or the future GUI polls to surface due reminders and mark them consumed.

---

## 4. Lifecycle & State Machine Rules

### 4.1 Update overwrite rule

Modifying fields (tags, description, etc.) overwrites existing data; it does not append. Passing an empty string (`""`) clears an optional field.

### 4.2 Recurrence — clone-on-complete

1. **Trigger** — user completes a task whose RecurrenceRule ≠ None.
2. **Original mutation** — original Status → Done, IsArchived → True.
3. **Clone generation** — new UUID (6-char prefix unique); copy Title, Description, Tags, Priority, BoardID, RecurrenceRule, Duration, and Reminders. Each cloned reminder's Fired flag resets to false.
4. **Time shift** — compute new DueDate via the recurrence algorithm (§4.3).
5. **Clone insertion** — Clone Status → ToDo; persist both atomically.

### 4.3 Recurrence date algorithm (fixed-anchor + roll-forward)

The next due date is anchored to the original due date and rolled forward one interval at a time until it lands in the future — never producing an already-overdue clone, never piling up missed occurrences.

**Fixed intervals (Daily / Weekly / Hourly)** have constant length, so the roll-forward is computed in closed form (O(1), no loop):

```
intervals_to_skip = ceil((now - original_due) / interval_length)
next_due = original_due + intervals_to_skip * interval_length
```

**Calendar intervals (Monthly / Yearly):** month and year lengths are irregular, so these step one calendar unit at a time, always anchored to the ORIGINAL day-of-month/date, clamping to the last valid day each step, until the date is in the future.

> **Worked example:** A monthly task anchored to the 31st: Jan 31 → Feb 28 (clamped) → Mar 31 → Apr 30 (clamped). The clamp is re-applied against the original day (31) each step, so the schedule never drifts.

### 4.4 Overdue workflow

The derived Overdue condition restricts actions. Allowed: postpone (updates date, reverts to ToDo) or cancel (→ Cancelled). Hard delete is blocked.

### 4.5 Hard delete (discard) restriction

Discard permanently erases data. Checked at runtime: discard is allowed only when IsArchived == true OR Status == Done OR Status == Cancelled; otherwise reject with a validation error. (Archive is an orthogonal flag, not a status — a task can be archived in any status.)

### 4.6 Reopen (state resurrection)

`reopen <ID>` transitions a Done or Cancelled task back to ToDo. Everything else about the task — title, tags, due date, priority, board, subtasks — is left identical; only the status changes.

### 4.7 Re-parenting

A task's parent is set at creation (`-p ParentID`) and can be changed later with `todo update <ID> --parent <ID|"">`. Re-parenting is a **move**: the task's subtree moves with it (children stay attached), not a flatten.

- **Root sentinel** — `--parent ""` (or any empty/absent value) means top-level/root.
- **Cycle prevention** (checked in order): normalize empty → root (skip further checks if root); the new parent must not be the task itself; the new parent must exist; the new parent must not be one of the task's own descendants (an O(subtree) downward walk collects descendants). This prevents cycles at the source.
- **Destination limits** — subtask depth ≤ 4 and ChildIDs ≤ 50 are still enforced on the destination after a move.

### 4.8 Reminder recalculation on due-date edit

When `todo update <ID> --due` changes the due date, every reminder that uses a duration offset has its fire time recomputed, and its Fired flag is reset to `false` **unconditionally**. If the new fire time is already in the past, the next `todo due-reminders` run fires it immediately — deciding that at edit time would be extra logic for no gain. Absolute-datetime reminders are unaffected by a due-date change.

---

## 5. System Integrity & Automated Failsafes

### 5.1 Board safe-delete

Board deletion is blocked while its TaskIDs array is non-empty. O(1) check.

### 5.2 Tree failsafe 1 — parent promotion

If a parent is discarded, iterate its ChildIDs and set each child's ParentID to `""`. Children are promoted to independent root tasks.

### 5.3 Tree failsafe 2 — child severing

If a child is discarded, locate its parent and remove the child's UUID from the parent's ChildIDs array.

### 5.4 Startup integrity routine (O(N))

Runs silently after hydration and auto-repairs structural inconsistencies. The repair destination is `storageBoardId` (a safety-net board distinct from the active board — §11.2).

1. **Orphan check** — if Task.BoardID is not found in boards.json, overwrite it with storageBoardId.
2. **Alignment check** — if a task has a ParentID, verify Task.BoardID == Parent.BoardID; if not, overwrite with the parent's BoardID.
3. **Depth check** — if a subtree exceeds depth 4, sever bidirectionally: clear the task's ParentID to `""`, remove the task from its parent's ChildIDs, and reassign the task's BoardID to storageBoardId (promoting it to a root task).
4. **Over-child check** — if a task has more than 50 children, collect it as a validation warning and block writes (cannot auto-repair without dropping relationships).

---

## 6. Identity, Safe Persistence & Data Safety

### 6.1 UUID generation & 6-char prefix uniqueness

Task/Board IDs are UUIDv4. The first 6 characters are the user-facing handle and must be unique. A prefix set is built once at hydration (O(1) lookups):

1. Generate a UUIDv4.
2. Take the first 6 characters; test against the existing-prefix set.
3. On collision, regenerate (loop). In practice this effectively never repeats.

### 6.2 Atomic writes

Persistence never overwrites a live file in place. Instead:

1. Serialize the full state to `<file>.tmp`.
2. Flush and `fsync` the temp file to disk.
3. Atomically rename `<file>.tmp` over the real file.

Because rename is atomic on all major platforms, the live file is always either wholly the old or wholly the new version — never partial. A crash mid-write leaves only a harmless `.tmp`, which is swept on next startup.

### 6.3 Undo journal (snapshot stack)

Mutations commit to the live database immediately. Separately, before each mutating write, a snapshot of the affected file (as it was before) is pushed onto a journal capped at the last 20 operations.

- `todo undo` restores the most recent snapshot back into the live database (itself an atomic write) and pops it from the stack.
- The stack-file mutation is also atomic, so a crash mid-undo cannot leave the journal and database disagreeing.
- **Command classification** — read-only commands (e.g. `list`, `show`, `next`, `upcoming`, `tree`) take no snapshot and do not commit; journaled mutations (e.g. `add`, `update`, `complete`, `discard`) snapshot before writing and commit; `due-reminders` is a **non-journaled mutation** — it commits `Fired=true` to disk but takes no snapshot, so a fired reminder is intentionally not undoable. (A double-fire on a crash between computing which reminders to fire and writing `Fired=true` is accepted for a CLI tool.)

### 6.4 Process lock (concurrency control)

To prevent two concurrent invocations from clobbering each other (last-writer-wins data loss), each process acquires an exclusive lock before reading:

- Acquire `.todo/.lock` exclusively (`O_CREAT | O_EXCL`). On success, proceed and delete on exit.
- If locked, retry every **50 ms**; if still locked after **2 s**, fail with a system/lock error exit code (§10.1).
- Stale-lock mitigation: the lock records PID + timestamp. Two independent reclamation signals — a **dead PID** (reclaim immediately) or a lock older than **30 s** (reclaim even if the PID appears live, as a backstop against PID recycling). Reclamation deletes the lock and re-acquires with `O_CREAT | O_EXCL` so two simultaneous reclaimers cannot both win.
- A `ProcessChecker` seam (parallel to the `Clock`/`IdGenerator` seams) makes liveness checks unit-testable and cross-platform.

### 6.5 Configurable data directory (TODO_HOME)

If the environment variable `TODO_HOME` is set, it overrides the data directory; otherwise the app falls back to `~/.todo/`. This enables safe automated testing (throwaway directories) and multiple independent task profiles.

---

## 7. Disaster Recovery (Rolling Backups)

Backups exist specifically for disaster recovery — restoring an entire task set after a bad bulk change or unexpected loss. They are distinct from atomic writes (which prevent corruption) and the undo journal (which reverses recent single actions).

- **Storage** — timestamped subfolders under `.todo/backups/`.
- **Trigger** — on backend execution, if the newest backup is older than 24 hours.
- **Routine** — plain-file copy of `tasks.json, boards.json, userdata.json` into a new timestamped folder (no zip / no external compression dependency).
- **Rotation** — keep a maximum of 7 backups; delete the oldest when creating the 8th.

---

## 8. Fuzzy Matching & Typo Tolerance

Fuzzy matching is a resolution fallback, not a standalone search engine. When a command references a task by ID or title (or a board by name) and the exact lookup fails, the input is matched against active titles/names using Levenshtein distance.

### 8.1 Resolution flow

1. Attempt exact match on the 6-char ID prefix (and title/name).
2. On miss, compute Levenshtein distance of the input against all active titles/board names.
3. Exactly one close match → prompt to confirm ("Did you mean 'X' (a3f9c1)? [y/N]") and proceed on yes.
4. Multiple close matches → list them and ask the user to choose.
5. No close match → error out.

### 8.2 Threshold

The edit-distance threshold scales with title length rather than using a flat cap, so short titles are not matched too loosely and long titles are not matched too strictly.

---

## 9. Validation Rules

All inputs are validated before mutation. Violations are rejected as input/state errors (exit code 1 — see §10.1). Field rules are stateless; relational rules (uniqueness, preconditions, re-parent cycles) live in the services.

| Field / Rule | Constraint |
|---|---|
| Title | 1–255 chars; non-empty (mandatory). |
| Description | ≤ 8,192 chars. |
| Priority | Integer 0–10. |
| Tags | ≤ 25 per task; each 1–50 chars; lowercased; `[a-z0-9-_]` only. |
| Subtask depth | ≤ 4 levels. |
| ChildIDs per task | ≤ 50. |
| Board name | 1–100 chars; unique. |
| DueDate | Must parse as ISO 8601; not in the past on manual entry. When Duration is set, DueDate must carry a time component. |
| Duration | Valid positive ISO 8601 duration; days/hours/minutes/seconds only — `P[n]Y` and `P[n]M` (months) are rejected with a "use days instead (e.g. P30D)" error. |
| Reminder | OffsetOrTime is a valid ISO 8601 duration OR absolute datetime; computed fire time must be before DueDate and in the future at creation. |
| Re-parent | (Relational, enforced in `TaskService`) the new parent is not the task itself, must exist, and must not be one of the task's descendants. Empty normalizes to root. |

---

## 10. CLI Specification

### 10.1 Exit codes

| Code | Meaning | Triggers |
|---|---|---|
| `0` | Success | Normal completion; read-only commands succeed even on dirty data. |
| `1` | Validation / input / state error | §9 field failure, state-machine violation, not-found, dirty-data blocks mutation. |
| `2` | System / I/O / lock error | JSON parse failure, lock timeout, fsync/rename failure, backup I/O failure. |
| `3` | User aborted | User answered `n` to a confirmation prompt (fuzzy-match confirm, confirm-discard). |

A future ImGui frontend needs to distinguish "command failed on bad input" (show an error) from "the user declined a confirmation" (do nothing) — hence the dedicated `3`. User-abort is signalled at the CLI/Formatter layer, not via the Error type.

### 10.2 I/O streams

stdout carries requested data exclusively. Warnings and errors go to stderr so a UI parser consuming stdout is never corrupted.

### 10.3 Global flags

- `-f, --format <type>` — output format (json | text).
- `-h, --help` — context-aware syntax documentation.

### 10.4 Workspace (board) commands

```
todo board add <"Name"> [--desc "Text"]
todo board switch <ID|Name>
todo board delete <ID|Name>
todo board list [--sort name|created|tasks] [--reverse]
```

### 10.5 Task commands

```
todo add <"Title"> [-d "Text"] [-b BoardID] [-p ParentID]
           [--due YYYY-MM-DD] [--duration <ISO duration>] [--reminder <offset|datetime>]
           [-r Rule] [-t "tags"] [-P 0-10]
todo list [--all] [-b BoardID] [-s Status] [--tag <tag>]
           [--archived] [--show-done] [--show-cancelled]
           [--sort due|priority|created|title|status] [--reverse]
todo show <ID>
todo tree [ID]
todo next
todo upcoming [--n <count>] [--within <ISO duration>]
todo due-reminders
todo update <ID> [--title "New"] [--desc "New"] [-s Status]
           [--due <YYYY-MM-DD>] [--duration <ISO duration>] [--reminder <offset|datetime>]
           [--parent <ID|"">] [-t "newtag"] [-P 0-10]
todo complete <ID>
todo reopen <ID>
todo cancel <ID>
todo postpone <ID> --date <YYYY-MM-DD>
todo archive <ID>
todo discard <ID>
todo undo
```

**Read & reminder commands:** `todo next` prints the first task by due date (tasks with no due date are ineligible; if none qualify, a warning goes to stderr and the command exits 0). `todo upcoming` shows the next N due/event tasks within a window (defaults `upcomingCount=5` / `upcomingWindow="P3D"` from settings — §11.2; `--n` / `--within` override per-invocation). `todo show <ID>` is a detailed single-task view; `todo tree [ID]` is an ASCII subtask hierarchy walk (with a cycle guard against corrupted data). `todo due-reminders` is a non-journaled mutation (§6.3) that an external scheduler polls — it surfaces reminders whose fire time has passed and Fired is false, then marks them fired.

### 10.6 Sorting

Sorting is a display concern applied only to listing commands (`todo list`, `todo board list`); it never changes stored order. `--sort` selects the key and `--reverse` flips the result. The default task sort is by due date with overdue items first. Priority sort orders P0 (emergency) first through P10 (non-issue).

> The default sort key is user-configurable via the `defaultSort` setting (§11.2); `--sort` overrides it per-invocation.

### 10.7 Configuration commands

```
todo config get <key>
todo config set <key> <value>
todo config list
```

Settings live in `userdata.json` as a typed `UserData` struct (§11.2) plus an `extra` map for UI-owned preferences the CLI passes through untouched. Known keys are validated against the typed fields; unknown keys go into `extra`. The `showArchived` / `showDone` / `showCancelled` settings are the defaults for the matching `todo list` flags, and `upcomingCount` / `upcomingWindow` are the defaults for `todo upcoming`.

---

## 11. Resolved Design Decisions

The sections formerly marked TBD were designed collaboratively and are now locked (2026-07-25/26).

### 11.1 Class & function design — resolved

The full object model — five layers (Domain → Core seams → Repository → Services → CLI), entities, error handling (`std::expected<T, Error>`), the validation/recurrence/duration namespaces, param structs, the `Resolver`, `TaskService` / `BoardService`, and the `Application` lifecycle — is specified in `PowerTree_Class_Design.md`, with code signatures in `PowerTree_Sketches.h`. Toolchain: CMake + vcpkg manifest mode, GCC 13.4 / MinGW-w64, **C++23** (`CMAKE_CXX_STANDARD 23`, `REQUIRED ON`, `EXTENSIONS OFF`); libraries nlohmann-json, cli11, fmt/std::format.

### 11.2 Settings / configuration system — resolved

User preferences are stored in `userdata.json` as a typed `UserData` struct (edited via `todo config` — §10.7):

| Field | Default | Purpose |
|---|---|---|
| `activeBoardId` | — | Board new tasks land on when `-b` is not given (set via `todo board switch`). |
| `storageBoardId` | — | Safety-net destination for §5.4 integrity repair; not a normal workflow destination. |
| `defaultSort` | `"due"` | Default `todo list` sort key. |
| `defaultPriority` | `5` | Priority applied when `-P` is not given. |
| `defaultRecurrenceInterval` | `1` | Default recurrence interval. |
| `theme` | `"default"` | UI theme (future frontend). |
| `dateFormat` | `"YYYY-MM-DD"` | Date display format. |
| `confirmDiscard` | `true` | Whether `discard` prompts for confirmation. |
| `showArchived` | `false` | Default for `todo list --archived`. |
| `showDone` | `false` | Default for `todo list --show-done`. |
| `showCancelled` | `false` | Default for `todo list --show-cancelled`. |
| `upcomingCount` | `5` | Default N for `todo upcoming`. |
| `upcomingWindow` | `"P3D"` | Default window for `todo upcoming`. |
| `extra` | — | UI-owned preferences; CLI passes through untouched. |

### 11.3 Lock-file mechanics — resolved

See §6.4 for the concrete parameters: 50 ms retry cadence, 2 s acquire timeout, 30 s stale-age cap, dead-PID immediate reclaim, and the `ProcessChecker` seam.

### 11.4 Exit-code finalization — resolved

See §10.1 for the finalized scheme (0 success, 1 validation/input/state, 2 system/I/O/lock, 3 user aborted).

### 11.5 Nice-to-have features — locked

The following were designed and locked during review and are reflected in the sections above: event-like tasks via `Duration` (§3.1, §3.4) with a hand-rolled ISO 8601 duration parser (no new dependency); `Reminders` and `todo due-reminders` (§3.4, §6.3, §10.5) with recalculation-on-due-edit (§4.8); `todo next` / `todo upcoming` / `todo show` / `todo tree` / `todo list --tag` (§10.5); re-parenting via `update --parent` (§4.7); and relative date input (`tomorrow`, `+3d`, `+2w`) expanded to ISO 8601 in the CLI layer.

---

## 12. Build & Dependency Toolchain

Dependencies are managed with Microsoft vcpkg in manifest mode. A `vcpkg.json` at the project root lets CMake automatically download, compile, and link dependencies on build.

### 12.1 Key libraries

| Library | Role |
|---|---|
| nlohmann-json | Serialize / deserialize tasks, boards, and user data. |
| cli11 | Parse terminal flags and required arguments cleanly. |
| fmt | Optimized, readable string formatting. (Optionally replaceable by C++20 std::format.) |

### 12.2 Notes

- Commits the project to a CMake + `vcpkg.json` setup.
- Toolchain locked at **C++23** (GCC 13.4 / MinGW-w64), so `std::format` is available; `fmt` is optional and may be dropped in favour of `std::format` to remove one dependency.
