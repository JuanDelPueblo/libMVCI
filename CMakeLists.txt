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
# Every test target compiles the driver sources directly; it never loads the
# shared library. The no-argument run is the hardware-free check. A focused
# regression test is one file named test/mvci_<area>_test.c and is registered
# automatically below; the build command for a standalone file is:
#
#     cmake -S . -B build/tests -DMVCI_BUILD_TESTS=ON
#     cmake --build build/tests
#     ctest --test-dir build/tests
#
# By default a test links test/mock_io.c, the in-memory scripted Mini-VCI
# device, and the provider sources (src/passthru.c, src/serial.c, src/des.c).
# A test that supplies its own mvci_io_* stubs is listed in the
# MVCI_OFFLINE_TESTS_NO_MOCK cache list (semicolon-separated names).
# test/<name>_support.c is compiled with its test when that file exists.
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

function(mvci_add_offline_test name)
    cmake_parse_arguments(_mvci_test "NO_MOCK" "" "" ${ARGN})

    set(_mvci_sources
        ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/passthru.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/serial.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/des.c)

    if(NOT _mvci_test_NO_MOCK)
        list(APPEND _mvci_sources ${CMAKE_CURRENT_SOURCE_DIR}/test/mock_io.c)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}_support.c)
        list(APPEND _mvci_sources ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}_support.c)
    endif()

    add_executable(${name} ${_mvci_sources})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    _mvci_link_test(${name})

    # No arguments: a hardware-free run.
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# --------------------------------------------------------------------------
# Self-test (links the protocol layer directly, not the J2534 exports)
# --------------------------------------------------------------------------
if(MVCI_BUILD_TESTS)
    enable_testing()
    add_executable(mvci_test
        test/mvci_test.c
        src/serial.c
        src/io.c
        src/des.c)
    target_include_directories(mvci_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

    if(WIN32)
        target_link_libraries(mvci_test PRIVATE bcrypt)
    elseif(APPLE)
        target_link_libraries(mvci_test PRIVATE Threads::Threads)
    else()
        target_link_libraries(mvci_test PRIVATE OpenSSL::Crypto Threads::Threads)
    endif()

    # No-argument run is the hardware-free codec self-test.
    add_test(NAME mvci_selftest COMMAND mvci_test)

    # J2534 provider sequence-equivalence test: the real PassThru* provider
    # against an in-memory scripted device (test/mock_io.c). Asserts the
    # exact emitted adapter-command sequences, so any extra background
    # traffic fails the comparison. No hardware.
    add_executable(mvci_seq_test
        test/mvci_seq_test.c
        test/mock_io.c
        src/passthru.c
        src/serial.c
        src/des.c)
    target_include_directories(mvci_seq_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

    if(WIN32)
        target_link_libraries(mvci_seq_test PRIVATE bcrypt)
    elseif(APPLE)
        target_link_libraries(mvci_seq_test PRIVATE Threads::Threads)
    else()
        target_link_libraries(mvci_seq_test PRIVATE OpenSSL::Crypto Threads::Threads)
    endif()

    add_test(NAME mvci_seqtest COMMAND mvci_seq_test)

    # Any other focused regression test in test/mvci_<area>_test.c is an
    # offline test with the same defaults. Integration does not edit this file
    # for a new test: add the standalone source and reconfigure. Put a test
    # that brings its own transport stubs in MVCI_OFFLINE_TESTS_NO_MOCK.
    set(MVCI_OFFLINE_TESTS_NO_MOCK "" CACHE STRING
        "test/mvci_<area>_test.c names that supply their own transport stubs")
    file(GLOB _mvci_extra_tests CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/test/mvci_*_test.c)
    foreach(_mvci_test_source ${_mvci_extra_tests})
        get_filename_component(_mvci_test_name ${_mvci_test_source} NAME_WE)
        if(_mvci_test_name STREQUAL "mvci_seq_test")
            continue()
        endif()
        if(_mvci_test_name IN_LIST MVCI_OFFLINE_TESTS_NO_MOCK)
            mvci_add_offline_test(${_mvci_test_name} NO_MOCK)
        else()
            mvci_add_offline_test(${_mvci_test_name})
        endif()
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
