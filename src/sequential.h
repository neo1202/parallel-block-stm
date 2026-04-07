#pragma once

// sequential.h - reference executor. Runs tx's one by one into a plain map;
// parallel output must match this for the same block + seed.

#include "transaction.h"

#include <unordered_map>

struct SequentialContext : ExecutionContext {
    std::unordered_map<Key, Value>& state;

    explicit SequentialContext(std::unordered_map<Key, Value>& s) : state(s) {}

    Value read(Key key) override {
        return state[key];
    }

    void write(Key key, Value value) override {
        state[key] = value;
    }
};

inline std::unordered_map<Key, Value> sequential_execute(
    const std::vector<Transaction>& block,
    const std::unordered_map<Key, Value>& initial_state
) {
    auto state = initial_state;
    SequentialContext ctx(state);
    for (const auto& tx : block) {
        tx.logic(ctx);
    }
    return state;
}
