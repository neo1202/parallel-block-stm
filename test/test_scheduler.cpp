#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "scheduler.h"

TEST_CASE("Scheduler: basic execution order") {
    Scheduler sched(3); // 3 transactions
    // separate claims = two different "threads". with batching, a single claim
    // would grab all 3 idx on first call and starve the second. in real runs
    // each worker has its own claim and CAS races spread them out.
    ExecClaim c1, c2, c3;

    auto t1 = sched.next_task(c1);
    REQUIRE(t1.has_value());
    CHECK(t1->kind == TaskKind::EXECUTION_TASK);
    CHECK(t1->version.txn_idx == 0);
    CHECK(t1->version.incarnation == 0);

    // release T1's claim by draining it into c1 (it'll return EXEC for idx 1/2
    // since batching claimed the whole block). we care that finish_execution
    // wires up validation, so do that first.
    sched.finish_execution(0, 0, true);

    // c2 is a fresh claim. block is already fully claimed via c1's refill,
    // so c2's next_version_to_execute won't refill. it'll fall to validation
    // since val_idx(0) < exec_idx(3).
    auto t2 = sched.next_task(c2);
    REQUIRE(t2.has_value());
    CHECK(t2->kind == TaskKind::VALIDATION_TASK);
    CHECK(t2->version.txn_idx == 0);

    auto v2 = sched.finish_validation(0, false);
    CHECK_FALSE(v2.has_value());
}

TEST_CASE("Scheduler: dependency and resume") {
    Scheduler sched(2); // 2 transactions
    ExecClaim c1, c2;

    auto t0 = sched.next_task(c1); // gets Exec(0) from claim [0,2)
    auto t1 = sched.next_task(c1); // same claim, drains to Exec(1)

    // T1 encounters a dependency on T0
    bool added = sched.add_dependency(1, 0);
    CHECK(added == true);

    // T0 finishes execution
    sched.finish_execution(0, 0, false); // no new location, so no sweeping validation needed
    
    // after resume, T1 is READY(1) and exec_idx was pulled back to 1 so
    // someone can re-claim it. the next task might be VAL(0) first (T0 is now
    // EXECUTED and awaiting validation) or EXEC(1, inc 1). either is correct
    // per the paper; exact order depends on which counter wins the race.
    auto t2 = sched.next_task(c2);
    REQUIRE(t2.has_value());
    if (t2->kind == TaskKind::VALIDATION_TASK) {
        // validation of T0 first, then EXEC(1, 1)
        CHECK(t2->version.txn_idx == 0);
        sched.finish_validation(0, false);
        auto t3 = sched.next_task(c2);
        REQUIRE(t3.has_value());
        CHECK(t3->kind == TaskKind::EXECUTION_TASK);
        CHECK(t3->version.txn_idx == 1);
        CHECK(t3->version.incarnation == 1);
    } else {
        CHECK(t2->kind == TaskKind::EXECUTION_TASK);
        CHECK(t2->version.txn_idx == 1);
        CHECK(t2->version.incarnation == 1);
    }
}

TEST_CASE("Scheduler: check done") {
    Scheduler sched(1);
    ExecClaim claim;

    CHECK(sched.done() == false);

    auto t0 = sched.next_task(claim);
    REQUIRE(t0.has_value());
    CHECK(t0->version.txn_idx == 0);

    sched.finish_execution(0, 0, true);

    auto v0 = sched.next_task(claim);
    REQUIRE(v0.has_value());
    CHECK(v0->kind == TaskKind::VALIDATION_TASK);

    sched.finish_validation(0, false); // Validated successfully

    auto none = sched.next_task(claim);
    CHECK_FALSE(none.has_value());

    // Call next_task to trigger check_done logic
    sched.next_task(claim);
    CHECK(sched.done() == true);
}
