find_package(GTest REQUIRED CONFIG)

add_executable(pagos_unit_tests
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
    unit/provenance_query_test.cpp unit/evidence_builder_test.cpp)
target_link_libraries(pagos_provenance_tests
    PRIVATE Pagos::provenance GTest::gtest_main pagos_project_options)
gtest_discover_tests(pagos_provenance_tests DISCOVERY_MODE PRE_TEST)

add_executable(pagos_cache_tests
    unit/cache_codec_test.cpp unit/cache_key_test.cpp unit/cache_store_test.cpp
    unit/stage_cache_test.cpp unit/evidence_view_test.cpp unit/function_evidence_test.cpp
    unit/stage_spill_test.cpp unit/query_checkpoint_test.cpp)
target_link_libraries(pagos_cache_tests
    PRIVATE Pagos::stage Pagos::mir GTest::gtest_main pagos_project_options)
gtest_discover_tests(pagos_cache_tests DISCOVERY_MODE PRE_TEST)

add_executable(pagos_cache_identity_probe cache_identity_probe.cpp)
target_link_libraries(pagos_cache_identity_probe
    PRIVATE Pagos::stage Pagos::codegen pagos_project_options)

add_executable(pagos_cache_store_probe cache_store_probe.cpp)
target_link_libraries(pagos_cache_store_probe
    PRIVATE Pagos::cache pagos_project_options)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME pagos-cache-identity-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cache_identity_process.py
            $<TARGET_FILE:pagos_cache_identity_probe>)
add_test(NAME pagos-cache-store-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cache_store_process.py
            $<TARGET_FILE:pagos_cache_store_probe>)
set_tests_properties(pagos-cache-store-process PROPERTIES TIMEOUT 60)
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
            $<TARGET_FILE:pagos_query_checkpoint_probe>)
# This matrix starts more than sixty compiler processes, including under ASan.
set_tests_properties(pagos-query-checkpoint-process PROPERTIES TIMEOUT 180)

find_program(PAGOS_LIT NAMES lit llvm-lit HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
find_program(PAGOS_FILECHECK NAMES FileCheck
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
find_program(PAGOS_TEST_CLANG NAMES clang
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lit)
configure_file(
    lit/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/lit/lit.site.cfg.py
    @ONLY)

add_test(
    NAME pagos-lit
    COMMAND ${PAGOS_LIT} -sv -j 1 ${CMAKE_CURRENT_BINARY_DIR}/lit)
