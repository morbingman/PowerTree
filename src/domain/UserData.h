// PowerTree — Domain layer: typed user settings (persisted in userdata.json).
// Known CLI settings are typed fields; `extra` holds UI-owned preferences the
// CLI passes through untouched. Edited via `todo config get/set/list`.

#pragma once

#include <string>
#include <unordered_map>

namespace powertree::domain {

struct UserData {
    std::string activeBoardId;                 // where new tasks land when -b is absent (set via `board switch`)
    std::string storageBoardId;                // safety-net destination for §5.4 integrity repair; not a workflow destination

    std::string defaultSort               = "due";
    int         defaultPriority           = 5;
    int         defaultRecurrenceInterval = 1;
    std::string theme                     = "default";
    std::string dateFormat                = "YYYY-MM-DD";

    bool        confirmDiscard            = true;
    bool        showArchived              = false;   // <-> `todo list --archived`
    bool        showDone                  = false;   // <-> `todo list --show-done`
    bool        showCancelled             = false;   // <-> `todo list --show-cancelled`

    int         upcomingCount             = 5;       // default N for `todo upcoming`
    std::string upcomingWindow            = "P3D";   // default window for `todo upcoming`

    std::unordered_map<std::string, std::string> extra;  // UI-owned prefs; CLI passes through untouched
};

} // namespace powertree::domain
