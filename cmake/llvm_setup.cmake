# LLVMConfig.cmake may select a CRT. Preserve an explicit caller selection.
set(PAGOS_REQUESTED_MSVC_RUNTIME "${CMAKE_MSVC_RUNTIME_LIBRARY}")
set(PAGOS_SAVED_PACKAGE_PREFERENCE "${CMAKE_FIND_PACKAGE_PREFER_CONFIG}")
if(WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" PAGOS_HOST_ARCHITECTURE)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT PAGOS_HOST_ARCHITECTURE STREQUAL "x64")
        message(FATAL_ERROR "The Windows host requires an x64 compiler; initialize an x64 Visual Studio developer shell")
    endif()
    # Use dependency export targets when the installed Windows packages supply
    # them; older LLVM Find modules assume different static library filenames.
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
endif()
find_package(LLVM REQUIRED CONFIG)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG "${PAGOS_SAVED_PACKAGE_PREFERENCE}")
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

if(WIN32 AND (MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    if(CMAKE_CONFIGURATION_TYPES)
        message(FATAL_ERROR
            "Windows hosts currently require a single-config generator; use the windows-* Ninja presets")
    endif()
    if(PAGOS_REQUESTED_MSVC_RUNTIME AND CMAKE_MSVC_RUNTIME_LIBRARY AND
       NOT PAGOS_REQUESTED_MSVC_RUNTIME STREQUAL CMAKE_MSVC_RUNTIME_LIBRARY)
        message(FATAL_ERROR
            "Requested CRT ${PAGOS_REQUESTED_MSVC_RUNTIME} conflicts with the LLVM SDK CRT ${CMAKE_MSVC_RUNTIME_LIBRARY}; select matching host dependencies")
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT LLVM_BUILD_TYPE STREQUAL "Debug")
        message(FATAL_ERROR
            "A Windows Debug build requires matching Debug LLVM/STL dependencies; use RelWithDebInfo with a Release SDK")
    endif()

    # Resolve SDK dependency targets with newer Windows package layouts. This
    # neither downloads dependencies nor substitutes SDK import libraries.
    if(LLVM_ENABLE_ZLIB AND NOT TARGET ZLIB::ZLIB)
        find_package(ZLIB QUIET CONFIG)
        if(TARGET ZLIB::ZLIBSTATIC AND NOT TARGET ZLIB::ZLIB)
            add_library(ZLIB::ZLIB ALIAS ZLIB::ZLIBSTATIC)
        endif()
        if(NOT TARGET ZLIB::ZLIB)
            find_package(ZLIB REQUIRED MODULE)
        endif()
    endif()
    if(LLVM_ENABLE_ZSTD)
        get_target_property(PAGOS_SUPPORT_DEPENDENCIES LLVMSupport INTERFACE_LINK_LIBRARIES)
        if(PAGOS_SUPPORT_DEPENDENCIES MATCHES "zstd::libzstd_static" AND
           NOT TARGET zstd::libzstd_static)
            find_package(zstd REQUIRED CONFIG)
            if(NOT TARGET zstd::libzstd_static)
                message(FATAL_ERROR "The LLVM SDK requires the zstd::libzstd_static target")
            endif()
        endif()
    endif()
    if(LLVM_ENABLE_DIA_SDK AND NOT TARGET DIASDK::Diaguids)
        find_package(DIASDK REQUIRED)
    endif()
    message(STATUS "Windows host CRT: ${CMAKE_MSVC_RUNTIME_LIBRARY}; LLVM configuration: ${LLVM_BUILD_TYPE}")
endif()

separate_arguments(PAGOS_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")

function(pagos_use_llvm_headers target)
    target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # C4702 is an optimizer diagnostic in instantiated LLVM ADT headers,
        # even with /external:W0. LLVM's HandleLLVMOptions disables it too.
        target_compile_options(${target} PRIVATE /wd4702)
    endif()
endfunction()
