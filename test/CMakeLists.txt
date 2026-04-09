# --- Test executables ---

# Correctness: parallel vs sequential
add_executable(test_correctness test_correctness.cpp)
target_link_libraries(test_correctness PRIVATE blockstm_core doctest)
add_test(NAME CorrectnessTest COMMAND test_correctness)

# MVMemory unit tests
add_executable(test_mvmemory test_mvmemory.cpp)
target_link_libraries(test_mvmemory PRIVATE blockstm_core doctest)
add_test(NAME MVMemoryTest COMMAND test_mvmemory)

# Scheduler unit tests
add_executable(test_scheduler test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE blockstm_core doctest)
add_test(NAME SchedulerTest COMMAND test_scheduler)

# Parallel correctness: parallel vs sequential
add_executable(test_parallel test_parallel.cpp)
target_link_libraries(test_parallel PRIVATE blockstm_core doctest)
add_test(NAME ParallelTest COMMAND test_parallel)
