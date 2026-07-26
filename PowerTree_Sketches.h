// PowerTree — Class Design Sketches
// Not compilable as-is; pseudocode/signatures for design review.
// Updated incrementally as each layer is locked.

#pragma once
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <expected>
#include <unordered_map>   // UserData::extra

// ============================================================
// NAMESPACE CONVENTION
// Each layer lives in its own namespace, matching the implementation:
//   powertree::domain   — entities, enums, ID wrappers, UserData
//   powertree::core     — Error, Clock, IdGenerator, ProcessChecker seams
//   powertree::repo     — Repository facade + JsonStore/LockFile/Journal/BackupManager
//   powertree::services — validation/recurrence/duration namespaces, services, param structs, Resolver
//   powertree::cli      — Formatter, Application, handler functions
// Cross-layer references below are left UNQUALIFIED for readability (e.g. Repository
// returns `Task`, not `powertree::domain::Task`). Real code qualifies them or uses
// using-declarations. This file is non-compilable design pseudocode.
// ============================================================

// ============================================================
// LAYER 1 — DOMAIN
// ============================================================

namespace powertree::domain {

enum class Status { ToDo, InProgress, Pending, Done, Cancelled };
enum class RecurrenceRule { None, Hourly, Daily, Weekly, Monthly, Yearly };

struct RecurrenceConfig {
    RecurrenceRule rule     = RecurrenceRule::None;
    int            interval = 1;   // e.g. 2 = every 2 weeks; extensible later
};

// Strong-typedef wrappers — prevent mixing TaskId/BoardId at compile time
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
// std::hash specializations needed for unordered_map keys (defined in .cpp)

// Reminder — shared optional value type; works on ANY task (event-like or not)
struct Reminder {
    std::string offsetOrTime;  // ISO 8601 duration ("PT15M" = 15m before dueDate) OR absolute ISO 8601 datetime
    bool        fired = false; // set by fire-layer once surfaced; data-only in CLI
};

struct Task {
    std::string                id;                            // full UUIDv4
    std::string                title;                         // 1–255
    std::string                description;                   // ≤8192
    int                        priority    = 5;               // 0–10
    Status                     status      = Status::ToDo;
    RecurrenceConfig           recurrence;
    std::string                parentId;                      // "" if root
    std::vector<std::string>   childIds;                      // ≤50
    std::string                boardId;
    std::string                createdAt;                     // ISO 8601 string
    std::string                dueDate;                       // ISO 8601 string, "" if none. Time component present => event start
    std::optional<std::string> duration;                      // ISO 8601 duration (e.g. "PT1H"); presence => event-like task. Computed end = dueDate + duration (never stored absolute)
    std::vector<Reminder>      reminders;                     // empty = none
    bool                       isArchived  = false;
    std::vector<std::string>   tags;                          // ≤25
};
// "Event" is not a separate entity: a task is event-like when dueDate carries a
// time component (start) and/or duration is set (end = dueDate + duration).
// Calendar = a view/filter over these.

struct Board {
    std::string              id;
    std::string              name;                            // 1–100, unique
    std::string              description;
    std::vector<std::string> taskIds;
};

} // namespace powertree::domain
// (std::hash specializations for TaskId/BoardId live at global scope — see Ids.h)

// ============================================================
// LAYER 2 — CORE SEAMS
// Toolchain: GCC 13.4, C++23, MinGW-w64
// CMakeLists.txt: CMAKE_CXX_STANDARD 23, REQUIRED ON, EXTENSIONS OFF
// ============================================================

namespace powertree::core {

// --- Error handling (std::expected<T, Error>) ---
enum class ErrorCode { Validation, NotFound, StateConflict, IoError, LockFailure, Dirty };
struct Error { ErrorCode code; std::string message; };
// Usage: std::expected<Task, Error>  std::expected<void, Error>
// CLI boundary maps ErrorCode -> exit code (§10.1)

// --- Clock seam (owns "now" + ISO 8601 parse/format) ---
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::system_clock::time_point now() const = 0;

    // ISO 8601 helpers — pure string<->time_point math, no "now" dependency
    static std::optional<std::chrono::system_clock::time_point>
        parseIso8601(const std::string&);
    static std::string
        toIso8601(std::chrono::system_clock::time_point);
};

class SystemClock : public Clock {
public:
    std::chrono::system_clock::time_point now() const override;
};

class FixedClock : public Clock {   // test double
    std::chrono::system_clock::time_point fixed_;
public:
    explicit FixedClock(std::chrono::system_clock::time_point t) : fixed_(t) {}
    std::chrono::system_clock::time_point now() const override { return fixed_; }
};

// --- IdGenerator seam (raw UUIDv4 only; collision-retry lives in Repository) ---
class IdGenerator {
public:
    virtual ~IdGenerator() = default;
    virtual std::string generate() const = 0;
};

class UuidGenerator : public IdGenerator {
public:
    std::string generate() const override;   // real UUIDv4
};

class SequentialIdGenerator : public IdGenerator {  // test double
    mutable int counter_ = 0;
public:
    std::string generate() const override;   // "000001", "000002", ...
};

} // namespace powertree::core

// ============================================================
// §11.4 — EXIT-CODE FINALIZATION
// ============================================================

// Finalized exit codes (supersedes provisional §10.1):
// 0 = Success
// 1 = Validation / input / state error (Validation, NotFound, StateConflict, Dirty)
// 2 = System / I/O / lock error (IoError, LockFailure)
// 3 = User aborted (answered 'n' to confirm/pick prompt)
// Note: user-abort is signalled at CLI/Formatter layer, not via Error struct.

// ============================================================
// §11.3 — LOCK-FILE MECHANICS
// ============================================================

// ProcessChecker seam — makes stale-lock reclamation unit-testable + cross-platform
namespace powertree::core {
class ProcessChecker {
public:
    virtual ~ProcessChecker() = default;
    virtual bool isAlive(long pid) const = 0;
};
class SystemProcessChecker : public ProcessChecker { ... };  // Windows: OpenProcess
class FakeProcessChecker   : public ProcessChecker { ... };  // test: scripted alive/dead
} // namespace powertree::core

// LockFile takes a ProcessChecker& (injected)
// Tuning: retry cadence=50ms, acquire timeout=2s, stale age cap=30s
// Stale signals: dead PID (immediate reclaim) OR age>30s (backstop)
// Reclamation: delete .lock, re-acquire with O_CREAT|O_EXCL

// ============================================================
// §11.2 — SETTINGS / CONFIGURATION
// ============================================================

namespace powertree::domain {  // UserData is a domain value type (persisted in userdata.json)

struct UserData {
    std::string activeBoardId;
    std::string storageBoardId;                // orphan/depth-repair destination (§5.4); not a workflow destination
    std::string defaultSort               = "due";
    int         defaultPriority           = 5;
    int         defaultRecurrenceInterval = 1;
    std::string theme                     = "default";
    std::string dateFormat                = "YYYY-MM-DD";
    bool        confirmDiscard            = true;
    bool        showArchived              = false;  // ↔ --archived flag
    bool        showDone                  = false;  // ↔ --show-done flag
    bool        showCancelled             = false;  // ↔ --show-cancelled flag
    int         upcomingCount             = 5;      // default N for `todo upcoming`
    std::string upcomingWindow            = "P3D";  // default window for `todo upcoming`
    std::unordered_map<std::string, std::string> extra;  // UI-owned prefs; CLI passes through untouched
};
} // namespace powertree::domain
// CLI surface: todo config get <key> | set <key> <value> | list
// Known keys validated against typed fields; unknown keys go into extra.

// ============================================================
// LAYER 3 — REPOSITORY
// Facade over four focused components. Services see only Repository.
// Lifecycle sequencing (lock→load→integrity→dispatch→commit→unlock)
// lives in Application (Layer 5), not here.
// ============================================================

namespace powertree::repo {

struct ValidationWarning {
    std::string entityId;   // task or board id
    std::string field;
    std::string message;
};

class JsonStore {
public:
    explicit JsonStore(std::filesystem::path filePath);
    nlohmann::json load();                       // lenient; throws only on malformed JSON / missing PK
    void           save(const nlohmann::json&);  // atomic: write .tmp → fsync → rename
    void           sweepTmp();                   // delete <file>.tmp if present
};

class LockFile {
public:
    explicit LockFile(std::filesystem::path lockPath);
    void acquire();  // O_CREAT|O_EXCL; writes PID+timestamp; reclaims stale lock
    void release();  // deletes .lock
};

class Journal {
public:
    explicit Journal(std::filesystem::path journalPath);
    void                       push(const std::string& snapshot); // prepend; trim to cap 20; atomic write
    std::optional<std::string> pop();                             // return+remove top; atomic write; nullopt if empty
};

class BackupManager {
public:
    explicit BackupManager(std::filesystem::path backupDir);
    // if newest backup >24h old: copy tasks/boards/userdata → timestamped folder; rotate keep-7
    void runIfDue(const std::filesystem::path& dataDir, const Clock&);
};

class Repository {
public:
    Repository(std::filesystem::path dataDir,
               std::unique_ptr<IdGenerator>,
               std::unique_ptr<Clock>);

    // Lifecycle — called by Application in order
    void acquireLock();
    void load();          // hydrate + build prefix maps + collect ValidationWarnings
    void runIntegrity();  // §5.4 silent auto-repairs
    void release();

    // Dirty / warning surface
    bool isDirty() const;
    const std::vector<ValidationWarning>& loadWarnings() const;

    // Task CRUD
    std::expected<Task, Error> getTask(const TaskId&) const;
    std::expected<void, Error> addTask(Task);
    std::expected<void, Error> updateTask(const Task&);
    std::expected<void, Error> removeTask(const TaskId&);
    std::vector<Task>          allTasks() const;

    // Board CRUD
    std::expected<Board, Error> getBoard(const BoardId&) const;
    std::expected<void,  Error> addBoard(Board);
    std::expected<void,  Error> updateBoard(const Board&);
    std::expected<void,  Error> removeBoard(const BoardId&);
    std::vector<Board>          allBoards() const;

    // Userdata (typed UserData per §11.2; activeBoardId/storageBoardId live inside it)
    UserData    getUserData() const;
    void        setUserData(const UserData&);

    // ID generation — collision-retry on 6-char prefix lives here
    std::string generateId();

    // Persistence
    void snapshot(); // push current state to Journal before a mutation (called explicitly by Application)
    void commit();   // atomic write tasks/boards/userdata; trigger backup if due
    void undo();     // pop Journal snapshot → atomic restore
};

} // namespace powertree::repo

// ============================================================
// LAYER 4 — SERVICES
// Business logic: state machine (§4), recurrence (§4.2-4.3),
// tree failsafes (§5). Field validation via stateless `validation`.
// Relational rules (uniqueness, preconditions) live in the services.
// ============================================================

namespace powertree::services {
// validation / recurrence / duration are nested namespaces inside powertree::services.

// --- Stateless field validation (§9) ---
namespace validation {
    std::expected<void, Error> title(const std::string&);         // 1–255
    std::expected<void, Error> description(const std::string&);   // ≤8192
    std::expected<void, Error> priority(int);                     // 0–10
    std::expected<void, Error> tags(const std::vector<std::string>&); // ≤25, each 1–50, [a-z0-9-_]
    std::expected<void, Error> boardName(const std::string&);     // 1–100 (uniqueness = BoardService)
    std::expected<void, Error> dueDate(const std::string&, const Clock&); // ISO 8601, not past
    std::expected<void, Error> duration(const std::string&);     // valid POSITIVE ISO 8601 duration, D/H/M/S only (reject P[n]Y, P[n]M months)
    std::expected<void, Error> reminder(const std::string&);      // valid ISO 8601 duration OR absolute datetime
    // Relational rules live in TaskService (need repo + dueDate context) — NOT in this stateless namespace:
    //   - dueDate must carry a time component when duration is set
    //   - reminder fire time (dueDate - offset, or absolute) must be before dueDate and (on create) in the future
    //   - re-parent checks (normalize→root, no self-parent, parent exists, parent not a descendant) — see TaskService::checkReparent
    //   - (endTime > dueDate check dropped: positive duration => computed end always after start)
}

// --- Recurrence date math (§4.3) — pure, testable free functions ---
namespace recurrence {
    // fixed-anchor roll-forward to next future occurrence
    std::string nextDueDate(const RecurrenceConfig&,
                            const std::string& anchorDate,
                            const Clock&);
}

// --- ISO 8601 duration parse + apply — pure, testable, no new dependency ---
namespace duration {
    struct Parsed {
        int days    = 0;
        int hours   = 0;
        int minutes = 0;
        int seconds = 0;
    };
    // Parse "P[n]DT[n]H[n]M[n]S" combos. T-separator state disambiguates minutes
    // (PT1M) from months (P1M). Reject P[n]Y / P[n]M(months) with "use days instead".
    std::expected<Parsed, Error>                  parse(const std::string&);
    std::chrono::system_clock::time_point         apply(std::chrono::system_clock::time_point,
                                                        const Parsed&);   // end = dueDate + duration
}

// --- Command param structs ---
struct AddTaskParams {
    std::string                             title;              // required
    std::optional<std::string>              description;
    std::optional<std::string>              boardId;
    std::optional<std::string>              parentId;
    std::optional<std::string>              dueDate;
    std::optional<std::string>              duration;           // ISO 8601 duration; dueDate must have time component if set
    std::optional<std::vector<Reminder>>    reminders;          // offset or absolute; fire time must be < dueDate and future on create
    std::optional<RecurrenceConfig>         recurrence;
    std::optional<std::vector<std::string>> tags;
    int                                     priority = 5;       // default per spec
};

struct UpdateTaskParams {
    std::optional<std::string>              title;
    std::optional<std::string>              description;
    std::optional<Status>                   status;             // ToDo forbidden here (reopen only)
    std::optional<std::string>              dueDate;            // changing this recalc/reminders + resets fired (see TaskService::update)
    std::optional<std::string>              duration;
    std::optional<std::vector<Reminder>>    reminders;
    std::optional<std::string>              parentId;           // re-parent; "" = root sentinel (empty/absent normalizes to "")
    std::optional<std::vector<std::string>> tags;
    std::optional<int>                      priority;
};

struct ListFilter {
    bool                       all      = false;
    std::optional<std::string> boardId;
    std::optional<Status>      status;
    std::optional<std::string> tag;       // `todo list --tag <tag>` filter
    bool                       archived = false;
    // sort spec (key + reverse) — see Formatter/CLI layer
};

// --- Resolver (§8): produces candidates; CLI does confirm/pick interaction ---
struct ResolveResult {
    enum class Kind { Exact, Ambiguous, None } kind;
    std::vector<std::string> candidateIds;   // 1 = exact; >1 = ambiguous; 0 = none
};
class Resolver {
public:
    explicit Resolver(Repository&);
    ResolveResult resolveTask(const std::string& input) const;   // exact prefix → Levenshtein
    ResolveResult resolveBoard(const std::string& input) const;  // exact id/name → Levenshtein
private:
    Repository& repo_;
};

// --- TaskService ---
class TaskService {
public:
    TaskService(Repository&, Clock&);

    std::expected<Task, Error> add(const AddTaskParams&);
    // On a dueDate change: recompute every duration-offset reminder's fire time and
    // reset fired=false UNCONDITIONALLY (next due-reminders run re-fires if already past).
    // On a parentId change: run checkReparent (normalize→root, no self-parent, parent exists,
    //   parent not a descendant — prevents cycles at the source); subtree moves with the task.
    std::expected<Task, Error> update(const TaskId&, const UpdateTaskParams&);
    std::expected<void, Error> complete(const TaskId&);  // recurrence clone-on-complete (§4.2)
    std::expected<void, Error> reopen(const TaskId&);    // Done/Cancelled → ToDo only
    std::expected<void, Error> cancel(const TaskId&);
    std::expected<void, Error> postpone(const TaskId&, const std::string& newDate);
    std::expected<void, Error> archive(const TaskId&);   // flips IsArchived
    std::expected<void, Error> discard(const TaskId&);   // precond: archived||Done||Cancelled; tree cleanup (§5.2/5.3)
    std::expected<void, Error> undo();

    std::vector<Task> list(const ListFilter&) const;

    // Read-only queries (for the fire-layer / daily use)
    std::optional<Task>                next() const;                       // first by dueDate asc; no-due ineligible; nullopt = none qualify (handler warns + exit 0, NOT an error)
    std::vector<Task>                  upcoming(int n, const std::string& window) const; // N tasks within window by start time
    std::vector<Task>                  tree(const std::optional<TaskId>& root) const;   // pre-order subtask walk; visited-set cycle guard

private:
    std::expected<void, Error> transition(Task&, Status to);  // consults static transition table
    std::expected<void, Error> checkReparent(const Task&, const std::string& newParentId) const; // normalize→root, no self-parent, exists, not-a-descendant
    Repository& repo_;
    Clock&      clock_;
};

// --- BoardService ---
class BoardService {
public:
    BoardService(Repository&, Clock&);

    std::expected<Board, Error> add(const std::string& name, const std::string& desc);
    std::expected<void, Error>  switchTo(const BoardId&);
    std::expected<void, Error>  remove(const BoardId&);   // blocked if TaskIDs non-empty (§5.1)
    std::vector<Board>          list(/* BoardSortSpec */) const;

private:
    Repository& repo_;
    Clock&      clock_;
};

} // namespace powertree::services

// ============================================================
// LAYER 5 — CLI / APPLICATION
// Application owns the one-shot lifecycle. Handler functions
// wired via CLI11 subcommand callbacks. Formatter owns all output.
// ============================================================

namespace powertree::cli {

enum class Format { Text, Json };

class Formatter {
public:
    explicit Formatter(Format fmt,
                       std::ostream& out = std::cout,
                       std::ostream& err = std::cerr);

    void print(const Task&);
    void print(const std::vector<Task>&);
    void print(const Board&);
    void print(const std::vector<Board>&);
    void printWarnings(const std::vector<ValidationWarning>&);
    void printError(const Error&);

    // Resolver interaction — prompts to stderr, reads stdin
    bool                       confirm(const std::string& prompt); // "Did you mean X? [y/N]"
    std::optional<std::string> pick(const std::vector<std::string>& candidates);

private:
    Format        fmt_;
    std::ostream& out_;
    std::ostream& err_;
};

// Handler functions — one per command
// Each: resolve <ID> via Resolver + Formatter::confirm/pick,
//       build param struct, call service, format result, return exit code.
int handleAdd         (TaskService&, BoardService&, Resolver&, Formatter&, /* parsed args */);
int handleList        (TaskService&,                            Formatter&, /* parsed args */);
int handleUpdate      (TaskService&,                Resolver&, Formatter&, /* parsed args */);
int handleComplete    (TaskService&,                Resolver&, Formatter&, /* parsed args */);
int handleReopen      (TaskService&,                Resolver&, Formatter&, /* parsed args */);
int handleCancel      (TaskService&,                Resolver&, Formatter&, /* parsed args */);
int handlePostpone    (TaskService&,                Resolver&, Formatter&, /* parsed args */);
int handleArchive     (TaskService&,                Resolver&, Formatter&, /* parsed args */);
int handleDiscard     (TaskService&,                Resolver&, Formatter&, /* parsed args */);
int handleUndo        (TaskService&,                            Formatter&                  );
// Calendar / reminder queries — READ-ONLY (for external fire-layer / scheduler)
int handleUpcoming    (TaskService&,                            Formatter&, /* --n, --within (default from UserData) */);
// upcoming: next N due/event tasks by start time; --n/--within override UserData.upcomingCount/upcomingWindow
int handleNext        (TaskService&,                            Formatter&                  );
// next: first task by dueDate asc; no-due ineligible; warn to stderr + exit 0 if none
int handleShow        (TaskService&,                Resolver&, Formatter&, /* <id> */);
// show: detailed single-task view (all fields + children)
int handleTree        (TaskService&,                Resolver&, Formatter&, /* [id] */);
// tree: ASCII subtask hierarchy walk; visited-set cycle guard against corrupted JSON
int handleDueReminders(TaskService&,                            Formatter&                  );
// due-reminders: emit reminders whose fire time has passed && !fired; mark them fired=true (NON-journaled mutation)
int handleBoardAdd    (BoardService&,                           Formatter&, /* parsed args */);
int handleBoardSwitch (BoardService&,               Resolver&, Formatter&, /* parsed args */);
int handleBoardDelete (BoardService&,               Resolver&, Formatter&, /* parsed args */);
int handleBoardList   (BoardService&,                           Formatter&, /* parsed args */);

// Application — owns the one-shot lifecycle and all collaborators
class Application {
public:
    Application(std::filesystem::path dataDir,
                std::unique_ptr<Clock>,
                std::unique_ptr<IdGenerator>);

    int run(int argc, char** argv);  // returns exit code

    // Lifecycle (run() executes in this order):
    // 1. JsonStore::sweepTmp()
    // 2. repo_.acquireLock()
    // 3. repo_.load()
    // 4. repo_.runIntegrity()
    // 5. print loadWarnings() to stderr if any
    // 6. parse args (CLI11); block mutating commands if repo_.isDirty()
    // 7. repo_.snapshot()  (JOURNALED mutations only — NOT due-reminders)
    // 8. dispatch handler
    // 9. repo_.commit()    (any mutation — journaled AND non-journaled alike)
    // 10. repo_.release()
    // Command classes: read-only (no snapshot/no commit), journaled mutation
    //   (snapshot+commit), non-journaled mutation = due-reminders (commit only).
    // ErrorCode → exit code (§11.4): Validation/NotFound/StateConflict/Dirty → 1,
    //   IoError/LockFailure → 2, user-abort at CLI/Formatter layer → 3.

private:
    Repository   repo_;
    TaskService  tasks_;
    BoardService boards_;
    Resolver     resolver_;
    Formatter    formatter_;
};

} // namespace powertree::cli

