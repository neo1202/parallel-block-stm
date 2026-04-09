#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "mvmemory.h"
#include <unordered_map>

TEST_CASE("MVMemory: basic read and record") {
    std::unordered_map<Key, Value> initial = {{1, 100}, {2, 200}};
    MVMemory mem(5, initial);

    // Read from initial state (txn 0)
    auto res1 = mem.read(1, 0);
    CHECK(res1.status == ReadStatus::NOT_FOUND); // Since txn 0 is the lowest, nothing wrote before it

    // Txn 0 writes to key 1
    std::vector<ReadDescriptor> rs;
    std::vector<WriteDescriptor> ws = {{1, 150}};
    bool wrote_new = mem.record({0, 0}, rs, ws);
    CHECK(wrote_new == true); // First time writing to this location

    // Read from txn 1 (should see txn 0's write)
    auto res2 = mem.read(1, 1);
    CHECK(res2.status == ReadStatus::OK);
    CHECK(res2.value == 150);
    CHECK(res2.version.txn_idx == 0);
    CHECK(res2.version.incarnation == 0);

    // Read key 2 from txn 1 (nobody wrote to it yet)
    auto res3 = mem.read(2, 1);
    CHECK(res3.status == ReadStatus::NOT_FOUND);
}

TEST_CASE("MVMemory: estimates and read errors") {
    std::unordered_map<Key, Value> initial = {{1, 100}};
    MVMemory mem(5, initial);

    // Txn 0 writes to key 1
    mem.record({0, 0}, {}, {{1, 150}});

    // Txn 0 aborts -> writes converted to ESTIMATE
    mem.convert_writes_to_estimates(0);

    // Txn 1 tries to read key 1
    auto res = mem.read(1, 1);
    CHECK(res.status == ReadStatus::READ_ERROR);
    CHECK(res.blocking_txn_idx == 0);
}

TEST_CASE("MVMemory: validation logic") {
    std::unordered_map<Key, Value> initial = {{1, 100}};
    MVMemory mem(5, initial);

    // Txn 2 reads key 1 from storage
    std::vector<ReadDescriptor> rs2 = {{1, std::nullopt}};
    mem.record({2, 0}, rs2, {});

    // Validate Txn 2 -> should be valid
    CHECK(mem.validate_read_set(2) == true);

    // Txn 0 comes in and writes to key 1
    mem.record({0, 0}, {}, {{1, 150}});

    // Validate Txn 2 again -> should fail because T0 wrote to key 1
    CHECK(mem.validate_read_set(2) == false);
}
