// PowerTree — Core layer: IdGenerator seam.
// Produces a raw UUIDv4 only. Collision-retry on the 6-char prefix lives in
// Repository (it needs the in-memory prefix set). Keeping the generator dumb
// lets tests inject a deterministic sequence.
//
// Real impls (UuidGenerator, SequentialIdGenerator) are TODO — defined in
// IdGenerator.cpp during the Core build pass.

#pragma once

#include <string>

namespace powertree::core {

class IdGenerator {
public:
    virtual ~IdGenerator() = default;
    virtual std::string generate() const = 0;
};

class UuidGenerator : public IdGenerator {
public:
    std::string generate() const override;   // TODO: real UUIDv4
};

class SequentialIdGenerator : public IdGenerator {  // test double
    mutable int counter_ = 0;
public:
    std::string generate() const override;   // TODO: "000001", "000002", ...
};

} // namespace powertree::core
