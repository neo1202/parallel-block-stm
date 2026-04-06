#pragma once

// transaction.h - shared types. ExecutionContext is virtual so the same
// tx.logic lambda runs under both the sequential map and the parallel
// MVMemory. Read/write key sets are baked in at generation time - Block-STM
// has no opacity, so addresses must never depend on read values.

#include <cstdint>
#include <functional>
#include <vector>

using Key = uint64_t;
using Value = uint64_t;

struct ExecutionContext {
    virtual ~ExecutionContext() = default;
    virtual Value read(Key key) = 0;
    virtual void write(Key key, Value value) = 0;
};

struct Transaction {
    std::vector<Key> read_keys;
    std::vector<Key> write_keys;
    std::function<void(ExecutionContext&)> logic;
};
