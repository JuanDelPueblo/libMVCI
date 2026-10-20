cmake_minimum_required(VERSION 3.15)

project(MVCI
    VERSION 1.0.0
    DESCRIPTION "MVCI SAE J2534 PassThru driver (ISO14230 K-line)"
    LANGUAGES C)

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------
# When MVCI is pulled in via add_subdirectory() we default the extras off so
# an embedding project isn't burdened with our tests or install rules.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(_mvci_standalone ON)
else()
    set(_mvci_standalone OFF)
endif()

option(MVCI_BUILD_TESTS   "Build the MVCI codec self-test"       ${_mvci_standalone})
option(MVCI_INSTALL       "Generate MVCI install/export rules"   ${_mvci_standalone})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --------------------------------------------------------------------------
# The driver library (J2534 PassThru DLL / .so / .dylib)
# --------------------------------------------------------------------------
add_library(MVCI SHARED
    src/passthru.c
    src/serial.c
    src/io.c
    src/des.c)

# Aliases consumers can link — identical names to the installed package, so a
# vendored (add_subdirectory) and an installed (find_package) build are
# interchangeable in target_link_libraries():
#   libmvci           — plain name for add_subdirectory() consumers
#   libMVCI::libmvci  — namespaced name that find_package(libmvci) also provides
add_library(libmvci ALIAS MVCI)
add_library(libMVCI::libmvci ALIAS MVCI)

target_include_directories(MVCI
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(MVCI PRIVATE MVCI_BUILDING_DLL)

if(MSVC)
    target_compile_definitions(MVCI PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(MVCI PRIVATE
        -Wall -Wextra -Wno-deprecated-declarations)
endif()

# --------------------------------------------------------------------------
# Per-platform transport + crypto backends
# --------------------------------------------------------------------------
if(WIN32)
    # x86 -> MVCI32, x64 -> MVCI64, matching the original driver naming.
    # The architecture is whatever the active MSVC environment targets (it is
    # fixed per build tree), so the two DLLs come from two separate configures.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_mvci_arch "64")
    else()
        set(_mvci_arch "32")
    endif()
    set_target_properties(MVCI PROPERTIES OUTPUT_NAME "MVCI${_mvci_arch}")

    # The win32/win64 presets pass MVCI_EXPECT_ARCH so that running one from the
    # wrong Native Tools prompt fails here instead of silently building the
    # other architecture into the win32/win64 directory.
    if(DEFINED MVCI_EXPECT_ARCH AND NOT MVCI_EXPECT_ARCH STREQUAL _mvci_arch)
        if(MVCI_EXPECT_ARCH STREQUAL "32")
            set(_mvci_prompt "x86")
        else()
            set(_mvci_prompt "x64")
        endif()
        message(FATAL_ERROR
            "This preset targets ${MVCI_EXPECT_ARCH}-bit but the active MSVC "
            "environment is ${_mvci_arch}-bit.\n"
            "Open the \"${_mvci_prompt} Native Tools Command Prompt\" (or run "
            "'vcvarsall ${_mvci_prompt}') and re-run the preset.")
    endif()

    # Undecorated J2534 exports (shared by the 32- and 64-bit DLLs).
    if(MSVC)
        set_target_properties(MVCI PROPERTIES
            LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/src/mvci.def\"")
    else()  # MinGW
        target_sources(MVCI PRIVATE src/mvci.def)
    endif()

    # CNG/BCrypt for DES; ftd2xx.dll is loaded at runtime (no import lib).
    target_link_libraries(MVCI PRIVATE bcrypt)

elseif(APPLE)
    # DES via CommonCrypto (in libSystem — no link needed). Threads for the
    # keepalive; termios transport is part of the C library.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(MVCI PRIVATE Threads::Threads)

else()  # Linux / other Unix
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)   # libcrypto for DES
    target_link_libraries(MVCI PRIVATE OpenSSL::Crypto Threads::Threads)
endif()

set_target_properties(MVCI PROPERTIES
    C_VISIBILITY_PRESET hidden
    EXPORT_NAME libmvci               # -> libMVCI::libmvci when exported
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# --------------------------------------------------------------------------
# Offline test helpers.
#
# Every test target compiles the sources it exercises directly; it never loads
# the shared library and never touches hardware. test/offline-tests.txt is the
# single list of supported regressions with its exact sources, definitions,
# include directories and linker wraps. tools/check-offline.sh reads the same
# file, so the CMake, direct-compiler and sanitizer paths cannot drift.
#
# Add a regression by adding its source and one manifest line. There is no
# directory glob: a line that names a missing source is a configure error, not
# a silently reduced test set. The no-argument run is the hardware-free check.
# --------------------------------------------------------------------------
function(_mvci_link_test target)
    if(WIN32)
        target_link_libraries(${target} PRIVATE bcrypt)
    elseif(APPLE)
        target_link_libraries(${target} PRIVATE Threads::Threads)
    else()
        target_link_libraries(${target} PRIVATE OpenSSL::Crypto Threads::Threads)
    endif()
endfunction()

if(MVCI_BUILD_TESTS)
    enable_testing()

    set(_mvci_manifest "${CMAKE_CURRENT_SOURCE_DIR}/test/offline-tests.txt")
    if(NOT EXISTS "${_mvci_manifest}")
        message(FATAL_ERROR "Missing offline test manifest: ${_mvci_manifest}")
    endif()

    if(WIN32)
        set(_mvci_host win)
    elseif(APPLE)
        set(_mvci_host mac)
    else()
        set(_mvci_host gnu)
    endif()

    file(STRINGS "${_mvci_manifest}" _mvci_test_lines)
    foreach(_mvci_line IN LISTS _mvci_test_lines)
        string(STRIP "${_mvci_line}" _mvci_line)
        if(_mvci_line STREQUAL "" OR _mvci_line MATCHES "^#")
            continue()
        endif()

        string(REPLACE "|" ";" _mvci_fields "${_mvci_line}")
        list(LENGTH _mvci_fields _mvci_field_count)
        if(NOT _mvci_field_count EQUAL 8)
            message(FATAL_ERROR
                "test/offline-tests.txt: expected 8 '|'-separated fields: ${_mvci_line}")
        endif()
        list(GET _mvci_fields 0 _mvci_name)
        list(GET _mvci_fields 1 _mvci_test_name)
        list(GET _mvci_fields 2 _mvci_sources)
        list(GET _mvci_fields 3 _mvci_defines)
        list(GET _mvci_fields 4 _mvci_includes)
        list(GET _mvci_fields 5 _mvci_wraps)
        list(GET _mvci_fields 6 _mvci_hosts)
        list(GET _mvci_fields 7 _mvci_note)
        # CMake lists are ';'-separated; the manifest uses spaces.
        foreach(_mvci_var _mvci_sources _mvci_defines _mvci_includes _mvci_wraps)
            string(REPLACE " " ";" ${_mvci_var} "${${_mvci_var}}")
        endforeach()
        foreach(_mvci_var _mvci_defines _mvci_includes _mvci_wraps)
            if("${${_mvci_var}}" STREQUAL "-")
                set(${_mvci_var} "")
            endif()
        endforeach()

        set(_mvci_supported TRUE)
        if(_mvci_hosts STREQUAL "posix")
            if(_mvci_host STREQUAL "win")
                set(_mvci_supported FALSE)
            endif()
        elseif(_mvci_hosts STREQUAL "gnu")
            if(NOT _mvci_host STREQUAL "gnu")
                set(_mvci_supported FALSE)
            endif()
        elseif(NOT _mvci_hosts STREQUAL "any")
            message(FATAL_ERROR
                "test/offline-tests.txt: unknown host class '${_mvci_hosts}' for ${_mvci_name}")
        endif()
        if(NOT _mvci_supported)
            message(STATUS
                "MVCI: skipping offline test ${_mvci_name} on ${_mvci_host}: ${_mvci_note}")
            continue()
        endif()

        set(_mvci_target_sources "")
        foreach(_mvci_source IN LISTS _mvci_sources)
            set(_mvci_source_path "${CMAKE_CURRENT_SOURCE_DIR}/${_mvci_source}")
            if(NOT EXISTS "${_mvci_source_path}")
                message(FATAL_ERROR
                    "test/offline-tests.txt: ${_mvci_name} names missing source ${_mvci_source}")
            endif()
            list(APPEND _mvci_target_sources "${_mvci_source_path}")
        endforeach()

        add_executable(${_mvci_name} ${_mvci_target_sources})
        target_include_directories(${_mvci_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        foreach(_mvci_include IN LISTS _mvci_includes)
            target_include_directories(${_mvci_name} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/${_mvci_include})
        endforeach()
        if(_mvci_defines)
            target_compile_definitions(${_mvci_name} PRIVATE ${_mvci_defines})
        endif()
        foreach(_mvci_wrap IN LISTS _mvci_wraps)
            target_link_options(${_mvci_name} PRIVATE "-Wl,--wrap=${_mvci_wrap}")
        endforeach()
        _mvci_link_test(${_mvci_name})

        # No arguments: a hardware-free run.
        add_test(NAME ${_mvci_test_name} COMMAND ${_mvci_name})
    endforeach()
endif()

# --------------------------------------------------------------------------
# Install + export: consumers use
#   find_package(libmvci CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE libMVCI::libmvci)
# --------------------------------------------------------------------------
if(MVCI_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(_mvci_cmakedir ${CMAKE_INSTALL_LIBDIR}/cmake/libmvci)

    install(TARGETS MVCI
        EXPORT libmvciTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(DIRECTORY include/mvci
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT libmvciTargets
        FILE libmvciTargets.cmake
        NAMESPACE libMVCI::
        DESTINATION ${_mvci_cmakedir})

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/libmvciConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libmvciConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/libmvciConfig.cmake"
        INSTALL_DESTINATION ${_mvci_cmakedir})

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/libmvciConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/libmvciConfigVersion.cmake"
        DESTINATION ${_mvci_cmakedir})
endif()
