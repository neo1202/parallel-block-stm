# --- Benchmark executables ---

add_executable(bench_scaling bench_scaling.cpp)
target_link_libraries(bench_scaling PRIVATE blockstm_core)

add_executable(bench_contention bench_contention.cpp)
target_link_libraries(bench_contention PRIVATE blockstm_core)

add_executable(bench_blocksize bench_blocksize.cpp)
target_link_libraries(bench_blocksize PRIVATE blockstm_core)
