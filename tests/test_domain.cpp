// PowerTree — Domain layer sanity tests.
// Header-only structs, so these just exercise construction, defaults, and the
// strong-typedef wrappers. Compiled in the user's vcpkg build (Catch2 is not
// available in the Linux sandbox, so this file is not VM-compile-checked).

#include <catch2/catch_test_macros.hpp>
#include <functional>

#include "domain/Ids.h"
#include "domain/Entities.h"
#include "domain/UserData.h"

using namespace powertree::domain;

TEST_CASE("TaskId is a strong-typedef: equal ids compare equal, hash matches", "[domain][ids]") {
    TaskId a{"abc123"};
    TaskId b{"abc123"};
    TaskId c{"xyz789"};

    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
    REQUIRE(std::hash<TaskId>{}(a) == std::hash<TaskId>{}(b));
    REQUIRE(static_cast<const std::string&>(a) == "abc123");
}

TEST_CASE("BoardId equality and hash", "[domain][ids]") {
    BoardId a{"board-1"};
    BoardId b{"board-1"};
    REQUIRE(a == b);
    REQUIRE(std::hash<BoardId>{}(a) == std::hash<BoardId>{}(b));
}

TEST_CASE("Task defaults match the locked spec", "[domain][entities]") {
    Task t;
    REQUIRE(t.priority == 5);
    REQUIRE(t.status == Status::ToDo);
    REQUIRE(t.isArchived == false);
    REQUIRE(t.childIds.empty());
    REQUIRE(t.reminders.empty());
    REQUIRE_FALSE(t.duration.has_value());
    REQUIRE(t.dueDate.empty());
    REQUIRE(t.recurrence.rule == RecurrenceRule::None);
    REQUIRE(t.recurrence.interval == 1);
}

TEST_CASE("Reminder fired defaults to false", "[domain][entities]") {
    Reminder r{"-PT30M"};
    REQUIRE(r.fired == false);
    REQUIRE(r.offsetOrTime == "-PT30M");
}

TEST_CASE("UserData upcoming + display defaults", "[domain][userdata]") {
    UserData ud;
    REQUIRE(ud.upcomingCount == 5);
    REQUIRE(ud.upcomingWindow == "P3D");
    REQUIRE(ud.defaultSort == "due");
    REQUIRE(ud.defaultPriority == 5);
    REQUIRE(ud.confirmDiscard == true);
    REQUIRE(ud.showArchived == false);
    REQUIRE(ud.showDone == false);
    REQUIRE(ud.showCancelled == false);
}
