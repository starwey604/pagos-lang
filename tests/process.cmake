add_executable(pagos_cache_identity_probe cache_identity_probe.cpp)
target_link_libraries(pagos_cache_identity_probe
    PRIVATE Pagos::stage Pagos::codegen pagos_project_options)

add_executable(pagos_cache_store_probe cache_store_probe.cpp)
target_link_libraries(pagos_cache_store_probe
    PRIVATE Pagos::cache pagos_project_options)

add_executable(pagos_scalar_replay_probe scalar_replay_probe.cpp)
target_link_libraries(pagos_scalar_replay_probe PRIVATE Pagos::stage pagos_project_options)

add_executable(pagos_scalar_replay_cli_fixture scalar_replay_cli_fixture.cpp)
target_link_libraries(pagos_scalar_replay_cli_fixture PRIVATE Pagos::stage pagos_project_options)

add_executable(pagos_generator_replay_cli_fixture generator_replay_cli_fixture.cpp)
target_link_libraries(pagos_generator_replay_cli_fixture PRIVATE Pagos::stage pagos_project_options)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME pagos-host-paths-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/host_paths_process.py
            $<TARGET_FILE:pagosc>)
set_tests_properties(pagos-host-paths-process PROPERTIES TIMEOUT 120)
add_test(NAME pagos-cache-identity-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cache_identity_process.py
            $<TARGET_FILE:pagos_cache_identity_probe>)
add_test(NAME pagos-cache-store-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cache_store_process.py
            $<TARGET_FILE:pagos_cache_store_probe>)
set_tests_properties(pagos-cache-store-process PROPERTIES TIMEOUT 60)
add_test(NAME pagos-scalar-replay-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scalar_replay_process.py
            $<TARGET_FILE:pagos_scalar_replay_probe> $<TARGET_FILE:pagosc>)
set_tests_properties(pagos-scalar-replay-process PROPERTIES TIMEOUT 120)
add_test(NAME pagos-scalar-replay-cli-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scalar_replay_cli_process.py
            $<TARGET_FILE:pagosc> $<TARGET_FILE:pagos_scalar_replay_cli_fixture>)
set_tests_properties(pagos-scalar-replay-cli-process PROPERTIES TIMEOUT 180)

add_test(NAME pagos-generator-replay-cli-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generator_replay_cli_process.py
            $<TARGET_FILE:pagosc> $<TARGET_FILE:pagos_generator_replay_cli_fixture>)
set_tests_properties(pagos-generator-replay-cli-process PROPERTIES TIMEOUT 180)


add_executable(pagos_stage_spill_probe stage_spill_probe.cpp)
target_link_libraries(pagos_stage_spill_probe PRIVATE Pagos::stage pagos_project_options)
add_test(NAME pagos-stage-spill-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stage_spill_process.py
            $<TARGET_FILE:pagos_stage_spill_probe> $<TARGET_FILE:pagosc>)
set_tests_properties(pagos-stage-spill-process PROPERTIES TIMEOUT 60)
add_executable(pagos_query_checkpoint_probe query_checkpoint_probe.cpp)
target_link_libraries(pagos_query_checkpoint_probe PRIVATE Pagos::stage pagos_project_options)
add_test(NAME pagos-query-checkpoint-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/query_checkpoint_process.py
            $<TARGET_FILE:pagos_query_checkpoint_probe> --mode sources)
add_test(NAME pagos-query-checkpoint-witness-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/query_checkpoint_process.py
            $<TARGET_FILE:pagos_query_checkpoint_probe> --mode witness)
# Each matrix retains all 14 cases and the original deadline. ASan on a hosted
# runner must not share one timeout across the two independent matrices.
set_tests_properties(pagos-query-checkpoint-process
    pagos-query-checkpoint-witness-process PROPERTIES TIMEOUT 180)
add_test(NAME pagos-recursive-checkpoint-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recursive_checkpoint_process.py
            $<TARGET_FILE:pagos_query_checkpoint_probe>)
set_tests_properties(pagos-recursive-checkpoint-process PROPERTIES TIMEOUT 180)
add_test(NAME pagos-query-task-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/query_task_process.py
            $<TARGET_FILE:pagos_query_checkpoint_probe>)
set_tests_properties(pagos-query-task-process PROPERTIES TIMEOUT 180)
add_test(NAME pagos-topology-checkpoint-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/topology_checkpoint_process.py
            $<TARGET_FILE:pagos_query_checkpoint_probe>)
set_tests_properties(pagos-topology-checkpoint-process PROPERTIES TIMEOUT 180)
add_test(NAME pagos-query-task-benchmark-smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/benchmark_query_tasks.py
            --binary $<TARGET_FILE:pagos_query_task_bench> --smoke
            --output ${CMAKE_CURRENT_BINARY_DIR}/query-task-smoke.json)
set_tests_properties(pagos-query-task-benchmark-smoke PROPERTIES TIMEOUT 180)
add_test(NAME pagos-query-cli-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/query_cli_process.py
            $<TARGET_FILE:pagosc>)
set_tests_properties(pagos-query-cli-process PROPERTIES TIMEOUT 240)
add_test(NAME pagos-query-cli-stop-continue-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/query_cli_process.py
            $<TARGET_FILE:pagosc> --stop-continue)
set_tests_properties(pagos-query-cli-stop-continue-process PROPERTIES
    TIMEOUT 180 SKIP_RETURN_CODE 77)
add_test(NAME pagos-recursive-cli-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recursive_cli_process.py
            $<TARGET_FILE:pagosc>)
set_tests_properties(pagos-recursive-cli-process PROPERTIES TIMEOUT 180)
add_test(NAME pagos-recursive-task-cli-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/recursive_task_cli_process.py
            $<TARGET_FILE:pagosc>)
set_tests_properties(pagos-recursive-task-cli-process PROPERTIES TIMEOUT 180)

find_program(PAGOS_TEST_CLANG NAMES clang
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)

add_test(NAME pagos-generator-replay-effect-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generator_replay_effect_process.py
            $<TARGET_FILE:pagosc> ${PAGOS_TEST_CLANG}
            --abi=minimal-c
            --executable-suffix=${CMAKE_EXECUTABLE_SUFFIX})
add_test(NAME pagos-generator-replay-builtin-effect-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generator_replay_effect_process.py
            $<TARGET_FILE:pagosc> ${PAGOS_TEST_CLANG}
            --abi=builtin-entry
            --executable-suffix=${CMAKE_EXECUTABLE_SUFFIX})
set_tests_properties(pagos-generator-replay-effect-process
    pagos-generator-replay-builtin-effect-process PROPERTIES TIMEOUT 120)

find_program(PAGOS_TEST_AR NAMES llvm-ar HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
add_test(NAME pagos-c-interop-archive-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lit/Inputs/run_c_abi.py
        $<TARGET_FILE:pagosc> ${PAGOS_TEST_CLANG}
        --native-optimize --archive-tool=${PAGOS_TEST_AR}
        ${CMAKE_CURRENT_SOURCE_DIR}/lit/codegen/c-abi.pgs)
set_tests_properties(pagos-c-interop-archive-process PROPERTIES TIMEOUT 120)

# Reuse the native C interop fixtures with actual MSVC C callers/callees.
# Native O2 also links through a static archive; other modes link objects.
if(WIN32)
    find_program(PAGOS_TEST_MSVC NAMES cl REQUIRED)
    foreach(PAGOS_ABI_CASE c-abi pointers record-pointers volatile c-abi-storage)
        string(REPLACE "-" "_" PAGOS_ABI_HARNESS "${PAGOS_ABI_CASE}")
        set(PAGOS_ABI_OPTIONS)
        set(PAGOS_ABI_TRAPS trap)
        if(PAGOS_ABI_CASE STREQUAL "pointers")
            set(PAGOS_ABI_TRAPS read store discard)
        elseif(PAGOS_ABI_CASE STREQUAL "record-pointers")
            set(PAGOS_ABI_TRAPS read first discard store volatile late alignment)
        elseif(PAGOS_ABI_CASE STREQUAL "volatile")
            set(PAGOS_ABI_TRAPS null-read null-store misaligned-read misaligned-store discard-null)
        elseif(PAGOS_ABI_CASE STREQUAL "c-abi-storage")
            set(PAGOS_ABI_OPTIONS --no-traps)
        endif()
        foreach(PAGOS_ABI_MODE legacy native external)
            set(PAGOS_ABI_OPTIMIZATION)
            if(PAGOS_ABI_MODE STREQUAL "native")
                set(PAGOS_ABI_OPTIMIZATION --native-optimize --archive-tool=${PAGOS_TEST_AR})
            elseif(PAGOS_ABI_MODE STREQUAL "external")
                set(PAGOS_ABI_OPTIMIZATION --optimize)
            endif()
            set(PAGOS_ABI_TEST "pagos-c-interop-msvc-${PAGOS_ABI_CASE}-${PAGOS_ABI_MODE}")
            add_test(NAME ${PAGOS_ABI_TEST}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lit/Inputs/run_c_abi.py
                    $<TARGET_FILE:pagosc> ${PAGOS_TEST_CLANG}
                    --c-driver=msvc --c-compiler=${PAGOS_TEST_MSVC}
                    ${PAGOS_ABI_OPTIMIZATION} ${PAGOS_ABI_OPTIONS}
                    ${CMAKE_CURRENT_SOURCE_DIR}/lit/codegen/${PAGOS_ABI_CASE}.pgs
                    ${CMAKE_CURRENT_SOURCE_DIR}/lit/Inputs/${PAGOS_ABI_HARNESS}.c
                    ${PAGOS_ABI_TRAPS})
            set_tests_properties(${PAGOS_ABI_TEST} PROPERTIES TIMEOUT 120)
        endforeach()
    endforeach()
endif()

