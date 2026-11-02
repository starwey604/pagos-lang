# Supported CMake releases map MSVC's cxx_std_23 to latest. Bound the mode to
# C++23, then check the compiler and standard library actually selected.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    if(MSVC_VERSION LESS 1943)
        message(FATAL_ERROR "The Windows MSVC host requires MSVC 19.43 or newer with C++23 mode")
    elseif(MSVC_VERSION LESS 1952)
        set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
    else()
        set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23")
    endif()
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "${CMAKE_CXX23_STANDARD_COMPILE_OPTION}")
endif()

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(CMAKE_REQUIRED_FLAGS "/EHsc /Zc:__cplusplus")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # try_compile initializes a fresh compiler module, so carry our bounded
    # mode through an imported target as well as the parent compiler mapping.
    add_library(pagos_cxx23_probe INTERFACE IMPORTED)
    set_property(TARGET pagos_cxx23_probe PROPERTY
        INTERFACE_COMPILE_OPTIONS "${CMAKE_CXX23_STANDARD_COMPILE_OPTION}")
    set(CMAKE_REQUIRED_LIBRARIES pagos_cxx23_probe)
endif()
check_cxx_source_compiles([[
    #include <expected>
    #include <fstream>
    #include <print>
    #include <version>
    static_assert(__cplusplus >= 202302L && __cplusplus < 202400L);
    static_assert(__cpp_lib_expected >= 202202L);
    static_assert(__cpp_lib_print >= 202207L);
    static_assert(__cpp_lib_ios_noreplace >= 202207L);
    int main() {
        std::expected<int, int> value{23};
        auto mode = std::ios::out | std::ios::noreplace;
        std::println("C++{}", *value);
        return mode == std::ios::openmode{};
    }
]] PAGOS_HAS_CXX23_LIBRARY)
cmake_pop_check_state()
if(NOT PAGOS_HAS_CXX23_LIBRARY)
    message(FATAL_ERROR
        "Pagos requires C++23 mode with std::expected, std::print, and ios::noreplace; check the compiler and standard library")
endif()
message(STATUS "Pagos C++23 mode: ${CMAKE_CXX23_STANDARD_COMPILE_OPTION}; frontend: ${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}")

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_GENERATOR MATCHES "Ninja")
    # Ninja compares /showIncludes bytes with CMake's detected prefix. Mixed
    # console input/output encodings can silently discard every header edge.
    set(PAGOS_DEPENDENCY_PROBE "${CMAKE_BINARY_DIR}/CMakeFiles/pagos-dependencies")
    file(MAKE_DIRECTORY "${PAGOS_DEPENDENCY_PROBE}")
    file(WRITE "${PAGOS_DEPENDENCY_PROBE}/依赖 检查.h" "// UTF-8 dependency probe\n")
    file(WRITE "${PAGOS_DEPENDENCY_PROBE}/probe.cpp" "#include \"依赖 检查.h\"\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /utf-8 /showIncludes /c probe.cpp /Foprobe.obj
        WORKING_DIRECTORY "${PAGOS_DEPENDENCY_PROBE}"
        RESULT_VARIABLE PAGOS_DEPENDENCY_RESULT
        OUTPUT_VARIABLE PAGOS_DEPENDENCY_OUTPUT
        ERROR_VARIABLE PAGOS_DEPENDENCY_ERROR
        ENCODING UTF-8)
    string(FIND "${PAGOS_DEPENDENCY_OUTPUT}" "${CMAKE_CL_SHOWINCLUDES_PREFIX}" PAGOS_PREFIX_POSITION)
    string(FIND "${PAGOS_DEPENDENCY_OUTPUT}" "依赖 检查.h" PAGOS_HEADER_POSITION)
    if(NOT PAGOS_DEPENDENCY_RESULT EQUAL 0 OR NOT CMAKE_CL_SHOWINCLUDES_PREFIX OR
       PAGOS_PREFIX_POSITION LESS 0 OR PAGOS_HEADER_POSITION LESS 0)
        message(FATAL_ERROR
            "MSVC/Ninja header dependency output is not consistent UTF-8. Set both console InputEncoding and OutputEncoding to UTF-8, then configure with --fresh; see docs/windows.md")
    endif()
endif()

add_library(pagos_project_options INTERFACE)
target_compile_features(pagos_project_options INTERFACE cxx_std_23)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(pagos_project_options INTERFACE
        /W4 /EHsc /utf-8 /Zc:__cplusplus /bigobj
        $<$<BOOL:${PAGOS_WARNINGS_AS_ERRORS}>:/WX>)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(pagos_project_options INTERFACE /permissive- /Zc:preprocessor)
    else()
        # clang-cl's -Wall is the /Wall alias, not Clang's usual -Wall.
        target_compile_options(pagos_project_options INTERFACE
            /clang:-Wall -Wextra -Wconversion -Wpedantic)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pagos_project_options INTERFACE
        -Wall -Wextra -Wconversion -Wpedantic
        $<$<BOOL:${PAGOS_WARNINGS_AS_ERRORS}>:-Werror>)
endif()
if(WIN32)
    target_compile_definitions(pagos_project_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pagos_project_options INTERFACE
        -Wshadow -Wno-missing-designated-field-initializers)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(pagos_project_options INTERFACE -Wno-missing-field-initializers)
endif()
if(PAGOS_ENABLE_SANITIZERS)
    if(WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "The ASan/UBSan configuration requires a non-Windows Clang or GCC host")
    endif()
    target_compile_options(pagos_project_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pagos_project_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
