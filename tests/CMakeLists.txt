find_package(GTest REQUIRED CONFIG)

add_executable(pagos_unit_tests
    unit/evaluator_test.cpp
    unit/lexer_test.cpp
    unit/mir_test.cpp
    unit/parser_test.cpp
    unit/stage_test.cpp
    unit/source_manager_test.cpp)
target_link_libraries(pagos_unit_tests
    PRIVATE Pagos::syntax Pagos::vm Pagos::mir Pagos::stage GTest::gtest_main
            pagos_project_options)

include(GoogleTest)
gtest_discover_tests(pagos_unit_tests DISCOVERY_MODE PRE_TEST)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
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
