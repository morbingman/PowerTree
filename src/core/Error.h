// PowerTree — Core layer: error type.
// Services return std::expected<T, Error>; the CLI maps ErrorCode -> exit code.

#pragma once

#include <string>

namespace powertree::core {

enum class ErrorCode {
    Validation,    // a field rule (§9) failed
    NotFound,      // id/title lookup miss
    StateConflict, // state-machine violation or precondition failure
    IoError,       // JSON parse, fsync/rename, backup failure
    LockFailure,   // process-lock acquisition timeout
    Dirty          // mutating command blocked on unclean load data
};

struct Error {
    ErrorCode   code;
    std::string message;
};

} // namespace powertree::core
