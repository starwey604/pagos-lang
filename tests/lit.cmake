find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

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
set(PAGOS_TEST_COMPILER "$<TARGET_FILE:pagosc>")
set(PAGOS_LIT_EXEC_ROOT "${CMAKE_CURRENT_BINARY_DIR}/lit" CACHE PATH
    "lit working directory (may contain Unicode and spaces)")
configure_file(
    lit/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py.in
    @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/lit/lit.site.cfg.py"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py.in")

add_test(
    NAME pagos-lit
    COMMAND ${PAGOS_LIT} -sv -j 1 --show-unsupported
        --output ${CMAKE_CURRENT_BINARY_DIR}/lit-results.json
        ${CMAKE_CURRENT_BINARY_DIR}/lit)
# This is the entire serial lit suite, not one compiler invocation. Hosted
# ASan runs already approach the CI entry's default 300-second test deadline.
set_tests_properties(pagos-lit PROPERTIES TIMEOUT 600)
add_test(NAME pagos-lit-paths-process
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lit_paths_process.py
        $<TARGET_FILE:pagosc> ${PAGOS_FILECHECK} ${PAGOS_LIT} ${Python3_EXECUTABLE}
        ${PAGOS_TEST_CLANG} ${PAGOS_TEST_LLD} ${PAGOS_TEST_NM} ${PAGOS_TEST_OBJDUMP}
        ${LLVM_TOOLS_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/lit)
set_tests_properties(pagos-lit-paths-process PROPERTIES TIMEOUT 90)
