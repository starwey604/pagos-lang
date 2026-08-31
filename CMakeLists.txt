cmake_minimum_required(VERSION 3.30)

project(Pagos VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PAGOS_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSanitizer" OFF)
option(PAGOS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 22)
    message(FATAL_ERROR
        "Pagos currently requires LLVM 22.x; found ${LLVM_PACKAGE_VERSION}")
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

separate_arguments(PAGOS_LLVM_DEFINITIONS
    NATIVE_COMMAND "${LLVM_DEFINITIONS}")

add_library(pagos_project_options INTERFACE)
target_compile_features(pagos_project_options INTERFACE cxx_std_26)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pagos_project_options INTERFACE
        -Wall
        -Wextra
        -Wconversion
        -Wpedantic
        $<$<BOOL:${PAGOS_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pagos_project_options INTERFACE
        -Wshadow
        -Wno-missing-designated-field-initializers)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(pagos_project_options INTERFACE
        -Wno-missing-field-initializers)
endif()

if(PAGOS_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizers require Clang or GCC")
    endif()
    target_compile_options(pagos_project_options INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(pagos_project_options INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
endif()

add_library(pagos_source
    lib/source/diagnostic.cpp
    lib/source/source_manager.cpp)
add_library(Pagos::source ALIAS pagos_source)
target_include_directories(pagos_source PUBLIC include)
target_link_libraries(pagos_source PUBLIC pagos_project_options)

add_library(pagos_syntax
    lib/syntax/ast.cpp
    lib/syntax/lexer.cpp
    lib/syntax/parser.cpp
    lib/syntax/token.cpp)
add_library(Pagos::syntax ALIAS pagos_syntax)
target_include_directories(pagos_syntax PUBLIC include)
target_link_libraries(pagos_syntax PUBLIC Pagos::source pagos_project_options)

add_library(pagos_hir lib/hir/hir.cpp)
add_library(Pagos::hir ALIAS pagos_hir)
target_include_directories(pagos_hir PUBLIC include)
target_link_libraries(pagos_hir PUBLIC Pagos::source pagos_project_options)

add_library(pagos_vm lib/vm/evaluator.cpp)
add_library(Pagos::vm ALIAS pagos_vm)
target_include_directories(pagos_vm PUBLIC include)
target_link_libraries(pagos_vm PUBLIC Pagos::hir pagos_project_options)

add_library(pagos_sema lib/sema/type_checker.cpp)
add_library(Pagos::sema ALIAS pagos_sema)
target_include_directories(pagos_sema PUBLIC include)
target_link_libraries(pagos_sema
    PUBLIC Pagos::syntax pagos_project_options)

add_library(pagos_stage lib/stage/stage_analyzer.cpp)
add_library(Pagos::stage ALIAS pagos_stage)
target_include_directories(pagos_stage PUBLIC include)
target_link_libraries(pagos_stage
    PUBLIC Pagos::sema Pagos::hir Pagos::vm pagos_project_options)

add_library(pagos_mir
    lib/mir/lowering.cpp
    lib/mir/mir.cpp)
add_library(Pagos::mir ALIAS pagos_mir)
target_include_directories(pagos_mir PUBLIC include)
target_link_libraries(pagos_mir PUBLIC Pagos::hir pagos_project_options)

add_library(pagos_codegen lib/codegen/llvm_codegen.cpp)
add_library(Pagos::codegen ALIAS pagos_codegen)
target_include_directories(pagos_codegen PUBLIC include)
target_include_directories(pagos_codegen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(pagos_codegen PRIVATE ${PAGOS_LLVM_DEFINITIONS})
target_link_libraries(pagos_codegen
    PUBLIC Pagos::mir pagos_project_options)

if(TARGET LLVM AND LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(pagos_codegen PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(PAGOS_LLVM_LIBRARIES
        Core
        Support)
    target_link_libraries(pagos_codegen PRIVATE ${PAGOS_LLVM_LIBRARIES})
endif()

add_executable(pagosc tools/pagosc/main.cpp)
target_link_libraries(pagosc
    PRIVATE Pagos::syntax Pagos::sema Pagos::stage Pagos::mir Pagos::codegen
            pagos_project_options)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
