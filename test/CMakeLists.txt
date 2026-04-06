# --- Test executables ---

# Correctness: parallel vs sequential
add_executable(test_correctness test_correctness.cpp)
target_link_libraries(test_correctness PRIVATE blockstm_core doctest)

# MVMemory unit tests
add_executable(test_mvmemory test_mvmemory.cpp)
target_link_libraries(test_mvmemory PRIVATE blockstm_core doctest)

# Scheduler unit tests
add_executable(test_scheduler test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE blockstm_core doctest)
