find_package(GTest REQUIRED CONFIG)

add_executable(pagos_unit_tests
    unit/emission_test.cpp
    unit/c_abi_test.cpp
    unit/pointer_test.cpp
    unit/integer_test.cpp
    unit/value_test.cpp
    unit/type_test.cpp
    unit/target_test.cpp
    unit/hir_test.cpp
    unit/evaluator_test.cpp
    unit/lexer_test.cpp
    unit/mir_test.cpp
    unit/parser_test.cpp
    unit/stage_test.cpp
    unit/stage_provenance_test.cpp
    unit/provenance_snapshot_test.cpp
    unit/source_manager_test.cpp)
target_link_libraries(pagos_unit_tests
    PRIVATE Pagos::syntax Pagos::vm Pagos::mir Pagos::stage Pagos::codegen GTest::gtest_main
            pagos_project_options)

include(GoogleTest)
gtest_discover_tests(pagos_unit_tests DISCOVERY_MODE PRE_TEST)

add_executable(pagos_provenance_tests unit/provenance_test.cpp
    unit/provenance_query_test.cpp unit/provenance_recursive_test.cpp
    unit/evidence_builder_test.cpp)
target_link_libraries(pagos_provenance_tests
    PRIVATE Pagos::provenance GTest::gtest_main pagos_project_options)
gtest_discover_tests(pagos_provenance_tests DISCOVERY_MODE PRE_TEST)

add_executable(pagos_cache_tests
    unit/protocol_baseline_test.cpp
    unit/cache_codec_test.cpp unit/cache_key_test.cpp unit/cache_store_test.cpp
    unit/replay_descriptor_test.cpp unit/scalar_replay_test.cpp
    unit/scalar_replay_execution_test.cpp unit/generator_replay_test.cpp
    unit/generator_replay_record_test.cpp
    unit/stage_cache_test.cpp unit/evidence_view_test.cpp unit/function_evidence_test.cpp
    unit/stage_spill_test.cpp unit/query_checkpoint_test.cpp
    unit/recursive_checkpoint_test.cpp unit/task_store_test.cpp)
target_link_libraries(pagos_cache_tests
    PRIVATE Pagos::stage Pagos::mir GTest::gtest_main pagos_project_options)
gtest_discover_tests(pagos_cache_tests DISCOVERY_MODE PRE_TEST)

# temporary_directory.h uses LLVM Support directly in these unit suites.
foreach(PAGOS_TEST_TARGET pagos_unit_tests pagos_provenance_tests pagos_cache_tests)
    pagos_use_llvm_headers(${PAGOS_TEST_TARGET})
endforeach()

if(PAGOS_ENABLE_PROCESS_TESTS)
    include(process.cmake)
endif()
if(PAGOS_ENABLE_LIT_TESTS)
    include(lit.cmake)
endif()
message(STATUS "Pagos tests: unit=ON; process=${PAGOS_ENABLE_PROCESS_TESTS}; lit=${PAGOS_ENABLE_LIT_TESTS}")
