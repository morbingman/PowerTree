// PowerTree — entry point.
// Scaffold shell: proves the Domain/Core headers compile and the structs are
// usable. Replaced by the Application lifecycle (lock -> load -> integrity ->
// dispatch -> commit -> unlock) once the CLI layer is built.

#include <fmt/core.h>

#include "domain/Entities.h"
#include "domain/Ids.h"
#include "domain/UserData.h"
#include "core/Error.h"
#include "core/Clock.h"
#include "core/IdGenerator.h"
#include "core/ProcessChecker.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    powertree::domain::Task t;
    t.title = "Welcome to PowerTree";
    powertree::domain::UserData ud;

    fmt::print("PowerTree v0.1.0 (scaffold)\n");
    fmt::print("Domain ready — sample task '{}', upcoming default N={}\n",
               t.title, ud.upcomingCount);
    return 0;
}
