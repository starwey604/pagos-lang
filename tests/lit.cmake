find_package(Python3 REQUIRED COMPONENTS Interpreter)

find_program(PAGOS_LIT NAMES lit llvm-lit HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
find_program(PAGOS_FILECHECK NAMES FileCheck
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
find_program(PAGOS_TEST_CLANG NAMES clang
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
find_program(PAGOS_TEST_LLD NAMES ld.lld
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
find_program(PAGOS_TEST_NM NAMES llvm-nm
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)
find_program(PAGOS_TEST_OBJDUMP NAMES llvm-objdump
    HINTS ${LLVM_TOOLS_BINARY_DIR} REQUIRED)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lit)
configure_file(
    lit/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/lit/lit.site.cfg.py
    @ONLY)

add_test(
    NAME pagos-lit
    COMMAND ${PAGOS_LIT} -sv -j 1 ${CMAKE_CURRENT_BINARY_DIR}/lit)
# This is the entire serial lit suite, not one compiler invocation. Hosted
# ASan runs already approach the CI entry's default 300-second test deadline.
set_tests_properties(pagos-lit PROPERTIES TIMEOUT 600)
