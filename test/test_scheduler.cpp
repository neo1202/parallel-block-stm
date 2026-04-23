#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "scheduler.h"

TEST_CASE("Scheduler: basic execution order") {
    Scheduler sched(3); // 3 transactions

    // 1. Thread 1 gets task
    auto t1 = sched.next_task();
    REQUIRE(t1.has_value());
    CHECK(t1->kind == TaskKind::EXECUTION_TASK);
    CHECK(t1->version.txn_idx == 0);
    CHECK(t1->version.incarnation == 0);

    // 2. Thread 2 gets task
    auto t2 = sched.next_task();
    REQUIRE(t2.has_value());
    CHECK(t2->kind == TaskKind::EXECUTION_TASK);
    CHECK(t2->version.txn_idx == 1);

    // 3. Thread 1 finishes execution of T0, wrote new location
    auto v1 = sched.finish_execution(0, 0, true);
    
    // Because T0 wrote a new location, it triggers a validation task for T0 (and later T1, T2)
    auto t3 = sched.next_task();
    REQUIRE(t3.has_value());
    CHECK(t3->kind == TaskKind::VALIDATION_TASK);
    CHECK(t3->version.txn_idx == 0);

    // 4. Finish validation of T0 successfully
    auto v2 = sched.finish_validation(0, false);
    CHECK_FALSE(v2.has_value());
}

TEST_CASE("Scheduler: dependency and resume") {
    Scheduler sched(2); // 2 transactions

    // T0 starts
    auto t0 = sched.next_task(); // gets Exec(0)
    
    // T1 starts
    auto t1 = sched.next_task(); // gets Exec(1)

    // T1 encounters a dependency on T0
    bool added = sched.add_dependency(1, 0);
    CHECK(added == true);

    // T0 finishes execution
    sched.finish_execution(0, 0, false); // no new location, so no sweeping validation needed
    
    // T1 should now be available for execution again because T0 resumed it
    auto t2 = sched.next_task();
    REQUIRE(t2.has_value());
    CHECK(t2->kind == TaskKind::EXECUTION_TASK);
    CHECK(t2->version.txn_idx == 1);
    CHECK(t2->version.incarnation == 1); // incarnation increased!
}

TEST_CASE("Scheduler: check done") {
    Scheduler sched(1);
    
    CHECK(sched.done() == false);
    
    auto t0 = sched.next_task();
    REQUIRE(t0.has_value());
    CHECK(t0->version.txn_idx == 0);
    
    sched.finish_execution(0, 0, true);
    
    auto v0 = sched.next_task();
    REQUIRE(v0.has_value());
    CHECK(v0->kind == TaskKind::VALIDATION_TASK);
    
    sched.finish_validation(0, false); // Validated successfully
    
    auto none = sched.next_task();
    CHECK_FALSE(none.has_value());
    
    // Call next_task to trigger check_done logic
    sched.next_task();
    CHECK(sched.done() == true);
}
