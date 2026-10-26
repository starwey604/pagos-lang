cmake_minimum_required(VERSION 3.30)

project(Pagos VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PAGOS_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSanitizer" OFF)
option(PAGOS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(PAGOS_USE_LLVM_DYLIB "Prefer the monolithic LLVM shared library" ON)

find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 22 AND NOT LLVM_VERSION_MAJOR EQUAL 23)
    message(FATAL_ERROR
        "Pagos currently requires LLVM 22.x or 23.x; found ${LLVM_PACKAGE_VERSION}")
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")
foreach(PAGOS_TARGET X86 RISCV ARM)
    if(NOT PAGOS_TARGET IN_LIST LLVM_TARGETS_TO_BUILD)
        message(FATAL_ERROR "Pagos preparation requires LLVM ${PAGOS_TARGET} backend")
    endif()
endforeach()

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

add_library(pagos_values lib/integer.cpp lib/value.cpp lib/sema/type.cpp)
add_library(Pagos::values ALIAS pagos_values)
target_include_directories(pagos_values PUBLIC include)
target_include_directories(pagos_values SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(pagos_values PRIVATE ${PAGOS_LLVM_DEFINITIONS})
target_link_libraries(pagos_values PUBLIC pagos_project_options)
if(PAGOS_USE_LLVM_DYLIB AND TARGET LLVM AND LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(pagos_values PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(PAGOS_SUPPORT_LIBRARIES Support)
    foreach(PAGOS_LIBRARY IN LISTS PAGOS_SUPPORT_LIBRARIES)
        if(NOT TARGET ${PAGOS_LIBRARY})
            message(FATAL_ERROR "Missing LLVM component ${PAGOS_LIBRARY}; use PAGOS_USE_LLVM_DYLIB=ON or a complete LLVM component SDK")
        endif()
    endforeach()
    target_link_libraries(pagos_values PRIVATE ${PAGOS_SUPPORT_LIBRARIES})
endif()

add_library(pagos_hir lib/hir/hir.cpp lib/hir/runtime_path.cpp)
add_library(Pagos::hir ALIAS pagos_hir)
target_include_directories(pagos_hir PUBLIC include)
target_link_libraries(pagos_hir PUBLIC Pagos::source Pagos::values pagos_project_options)

# Versioned, target-independent cache protocols; LLVM Support stays private.
add_library(pagos_cache lib/cache/codec.cpp lib/cache/key.cpp lib/cache/store.cpp
    lib/cache/task_store.cpp)
add_library(Pagos::cache ALIAS pagos_cache)
target_include_directories(pagos_cache PUBLIC include)
target_include_directories(pagos_cache SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(pagos_cache PUBLIC Pagos::values pagos_project_options)

# Compact provenance storage; construction and immutable queries are separate.
add_library(pagos_provenance lib/stage/provenance_graph.cpp
    lib/stage/provenance_query.cpp lib/stage/provenance_topology.cpp
    lib/stage/provenance_recursive.cpp
    lib/stage/recursive_checkpoint.cpp
    lib/stage/query_storage.cpp
    lib/stage/evidence_view.cpp lib/stage/evidence_topology.cpp
    lib/stage/evidence_builder.cpp
    lib/stage/evidence_session.cpp)
add_library(Pagos::provenance ALIAS pagos_provenance)
target_include_directories(pagos_provenance PUBLIC include)
target_include_directories(pagos_provenance SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(pagos_provenance
    PUBLIC pagos_project_options Pagos::cache PRIVATE Pagos::values)

add_executable(pagos_provenance_query_bench tools/provenance_query_bench/main.cpp)
target_link_libraries(pagos_provenance_query_bench
    PRIVATE Pagos::provenance pagos_project_options)

add_executable(pagos_provenance_bench tools/provenance_bench/main.cpp)
target_link_libraries(pagos_provenance_bench
    PRIVATE Pagos::provenance Pagos::hir pagos_project_options)

add_library(pagos_vm lib/vm/evaluator.cpp)
add_library(Pagos::vm ALIAS pagos_vm)
target_include_directories(pagos_vm PUBLIC include)
target_link_libraries(pagos_vm PUBLIC Pagos::hir pagos_project_options)

add_library(pagos_sema lib/sema/type_checker.cpp)
add_library(Pagos::sema ALIAS pagos_sema)
target_include_directories(pagos_sema PUBLIC include)
target_link_libraries(pagos_sema
    PUBLIC Pagos::syntax Pagos::values pagos_project_options)

add_library(pagos_stage lib/stage/stage_analyzer.cpp
    lib/stage/provenance_analysis.cpp lib/stage/provenance_snapshot.cpp lib/stage/cache_identity.cpp
    lib/stage/function_evidence.cpp lib/stage/function_evidence_view.cpp
    lib/stage/scalar_cache.cpp lib/stage/residual_template.cpp)
add_library(Pagos::stage ALIAS pagos_stage)
target_include_directories(pagos_stage PUBLIC include)
target_link_libraries(pagos_stage
    PUBLIC Pagos::sema Pagos::hir Pagos::vm Pagos::provenance Pagos::cache pagos_project_options)
target_include_directories(pagos_stage SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

add_executable(pagos_snapshot_bench tools/snapshot_bench/main.cpp)
target_link_libraries(pagos_snapshot_bench PRIVATE Pagos::stage pagos_project_options)

add_library(pagos_mir
    lib/mir/lowering.cpp
    lib/mir/mir.cpp)
add_library(Pagos::mir ALIAS pagos_mir)
target_include_directories(pagos_mir PUBLIC include)
target_link_libraries(pagos_mir PUBLIC Pagos::hir pagos_project_options)

add_library(pagos_codegen lib/codegen/llvm_codegen.cpp lib/codegen/target.cpp)
add_library(Pagos::codegen ALIAS pagos_codegen)
target_include_directories(pagos_codegen PUBLIC include)
target_include_directories(pagos_codegen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(pagos_codegen PRIVATE ${PAGOS_LLVM_DEFINITIONS})
target_link_libraries(pagos_codegen
    PUBLIC Pagos::mir pagos_project_options)

if(PAGOS_USE_LLVM_DYLIB AND TARGET LLVM AND LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(pagos_codegen PRIVATE LLVM)
else()
    llvm_map_components_to_libnames(PAGOS_LLVM_LIBRARIES
        Core
        CodeGen
        Support
        Target
        TargetParser
        MC
        X86
        RISCV
        ARM)
    foreach(PAGOS_LIBRARY IN LISTS PAGOS_LLVM_LIBRARIES)
        if(NOT TARGET ${PAGOS_LIBRARY})
            message(FATAL_ERROR "Missing LLVM component ${PAGOS_LIBRARY}; use PAGOS_USE_LLVM_DYLIB=ON or a complete LLVM component SDK")
        endif()
    endforeach()
    target_link_libraries(pagos_codegen PRIVATE ${PAGOS_LLVM_LIBRARIES})
endif()

add_executable(pagosc tools/pagosc/main.cpp tools/pagosc/query_tasks.cpp
    tools/pagosc/recursive_summaries.cpp)
target_link_libraries(pagosc
    PRIVATE Pagos::syntax Pagos::sema Pagos::stage Pagos::mir Pagos::codegen
            pagos_project_options)

# Conservative compatibility identity: source/configuration changes regenerate
# it, including dirty source changes. No timestamps or session addresses.
file(GLOB_RECURSE PAGOS_IDENTITY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/pagos/*.h"
    "${CMAKE_SOURCE_DIR}/lib/*.h" "${CMAKE_SOURCE_DIR}/lib/*.cpp"
    "${CMAKE_SOURCE_DIR}/tools/pagosc/*.cpp"
    "${CMAKE_SOURCE_DIR}/tools/pagosc/*.h"
    "${CMAKE_SOURCE_DIR}/tools/pagosc/*.h.in")
list(SORT PAGOS_IDENTITY_SOURCES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${PAGOS_IDENTITY_SOURCES})
set(PAGOS_IDENTITY_INPUT
    "${PROJECT_VERSION}|${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_CXX_COMPILER_TARGET}|${CMAKE_SYSROOT}|${LLVM_PACKAGE_VERSION}|${CMAKE_BUILD_TYPE}|${CMAKE_CXX_FLAGS}|${PAGOS_ENABLE_SANITIZERS}|${PAGOS_USE_LLVM_DYLIB}")
string(TOUPPER "${CMAKE_BUILD_TYPE}" PAGOS_IDENTITY_CONFIGURATION)
string(APPEND PAGOS_IDENTITY_INPUT "|${CMAKE_CXX_FLAGS_${PAGOS_IDENTITY_CONFIGURATION}}")
foreach(PAGOS_IDENTITY_SOURCE IN LISTS PAGOS_IDENTITY_SOURCES)
    file(SHA256 "${PAGOS_IDENTITY_SOURCE}" PAGOS_SOURCE_DIGEST)
    file(RELATIVE_PATH PAGOS_SOURCE_NAME "${CMAKE_SOURCE_DIR}" "${PAGOS_IDENTITY_SOURCE}")
    string(APPEND PAGOS_IDENTITY_INPUT "|${PAGOS_SOURCE_NAME}:${PAGOS_SOURCE_DIGEST}")
endforeach()
file(SHA256 "${CMAKE_SOURCE_DIR}/CMakeLists.txt" PAGOS_CMAKE_DIGEST)
string(APPEND PAGOS_IDENTITY_INPUT "|${PAGOS_CMAKE_DIGEST}")
string(SHA256 PAGOS_CACHE_BUILD_ID "${PAGOS_IDENTITY_INPUT}")
configure_file(tools/pagosc/build_identity.h.in generated/build_identity.h @ONLY)
target_include_directories(pagosc PRIVATE "${CMAKE_BINARY_DIR}/generated")

add_executable(pagos_function_evidence_bench tools/function_evidence_bench/main.cpp)
target_include_directories(pagos_function_evidence_bench PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pagos_function_evidence_bench PRIVATE Pagos::stage pagos_project_options)

add_executable(pagos_query_task_bench tools/query_task_bench/main.cpp)
target_include_directories(pagos_query_task_bench PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pagos_query_task_bench PRIVATE Pagos::stage pagos_project_options)

add_executable(pagos_stage_spill_bench tools/stage_spill_bench/main.cpp)
target_link_libraries(pagos_stage_spill_bench PRIVATE Pagos::stage pagos_project_options)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
