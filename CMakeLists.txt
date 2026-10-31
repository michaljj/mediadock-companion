cmake_minimum_required(VERSION 3.15)

# Pin the newest installed Windows SDK. The C++/WinRT projection differs
# between SDK versions (newer SDKs expose the modernized SMTC API surface),
# so pinning keeps the code building deterministically.
set(CMAKE_SYSTEM_VERSION 10.0.28000.0 CACHE STRING "Windows SDK version")
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(WinApp VERSION 0.1.0 LANGUAGES C CXX)

add_executable(WinApp src/main.cpp)

target_sources(WinApp PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/serializer.cpp
    src/COMPort.cpp)

target_include_directories(WinApp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc)
# C++/WinRT requires C++17 at minimum; we use C++20 for convenience.
target_compile_features(WinApp PRIVATE cxx_std_20)

# ---------------------------------------------------------------------------
# Unit tests (ctest -C Debug)
# ---------------------------------------------------------------------------
enable_testing()
add_executable(serializer_test tests/test_serializer.cpp)
target_sources(serializer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/serializer.cpp
    ../Soft/user_app/src/protocol.c)
target_include_directories(serializer_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ../Soft/user_app/inc)
target_compile_features(serializer_test PRIVATE cxx_std_20)
add_test(NAME serializer_test COMMAND serializer_test)

# Firmware-side COBS/CRC/TLV decoder regression tests. The decoder is deliberately
# freestanding C so the same sources compile both in the STM32 target and on a
# host toolchain without HAL/FreeRTOS dependencies.
add_executable(protocol_test
    tests/test_protocol.c
    ../Soft/user_app/src/protocol.c)
target_include_directories(protocol_test PRIVATE
    ../Soft/user_app/inc)
target_compile_features(protocol_test PRIVATE c_std_11)
add_test(NAME protocol_test COMMAND protocol_test)

# Windows SDK import libraries:
#  - windowsapp.lib : umbrella lib for WinRT (RoGetActivationFactory, etc.)
#  - ole32.lib      : classic COM (CoCreateInstance for MMDevice API)
# The WinRT headers themselves (winrt/Windows.*.h) are part of the installed
# Windows SDK and MSVC adds <SDK>/Include/<ver>/cppwinrt to the include path
# automatically for every C++ project - no manual include dirs needed.
#  - setupapi.lib  : SetupAPI, used to auto-detect the dock's CDC COM port.
target_link_libraries(WinApp PRIVATE
    windowsapp.lib
    ole32.lib
    setupapi.lib
)

# ---------------------------------------------------------------------------
# Developer tooling
# ---------------------------------------------------------------------------

# The MSBuild-based VS build injects the Windows SDK and C++/WinRT include
# paths implicitly (WindowsSDK_IncludePath). Tools that consume
# compile_commands.json (clang-tidy, clangd) do not, so add them explicitly -
# this also pins them to the exact SDK version the build uses.
set(WINAPP_SDK_ROOT "$ENV{WindowsSdkDir}")
if(WINAPP_SDK_ROOT STREQUAL "")
    set(WINAPP_SDK_ROOT "C:/Program Files (x86)/Windows Kits/10")
endif()
string(REGEX REPLACE "[/\\\\]$" "" WINAPP_SDK_ROOT "${WINAPP_SDK_ROOT}")
foreach(_sdk_subdir cppwinrt shared um winrt ucrt)
    if(EXISTS "${WINAPP_SDK_ROOT}/Include/${CMAKE_SYSTEM_VERSION}/${_sdk_subdir}")
        target_include_directories(WinApp SYSTEM PRIVATE
            "${WINAPP_SDK_ROOT}/Include/${CMAKE_SYSTEM_VERSION}/${_sdk_subdir}")
    endif()
endforeach()

# compile_commands.json in the project root.
#
# CMake's Visual Studio generator ignores CMAKE_EXPORT_COMPILE_COMMANDS (only
# the Makefile and Ninja generators implement it), so we generate the database
# with a secondary Ninja configure inside the VS dev environment and copy it
# here, where clangd/clang-tidy expect it:
#
#     cmake --build build --target compile-commands
#
# build/compdb is a scratch configure tree used only for this. Re-run whenever
# targets/flags change; the 'tidy' target below refreshes it automatically.
file(GLOB _cc_vcvars
    "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat")
find_program(CMAKE_NINJA_PROGRAM NAMES ninja)
if(NOT CMAKE_NINJA_PROGRAM)
    file(GLOB _cc_vs_ninja
        "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe")
    if(_cc_vs_ninja)
        list(GET _cc_vs_ninja 0 CMAKE_NINJA_PROGRAM)
    endif()
endif()

if(_cc_vcvars AND CMAKE_NINJA_PROGRAM)
    list(GET _cc_vcvars 0 _cc_vcvars)
    set(_cc_script "${CMAKE_CURRENT_BINARY_DIR}/generate_compile_commands.cmd")
    file(WRITE "${_cc_script}"
"@echo off\r\n"
"call \"${_cc_vcvars}\" || exit /b 1\r\n"
"\"${CMAKE_COMMAND}\" -S \"${CMAKE_CURRENT_SOURCE_DIR}\" -B \"${CMAKE_CURRENT_SOURCE_DIR}/build/compdb\" -G Ninja"
" -DCMAKE_MAKE_PROGRAM=\"${CMAKE_NINJA_PROGRAM}\""
" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || exit /b 1\r\n"
"\"${CMAKE_COMMAND}\" -E copy_if_different"
" \"${CMAKE_CURRENT_SOURCE_DIR}/build/compdb/compile_commands.json\""
" \"${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json\"\r\n")
    add_custom_target(compile-commands
        COMMAND "${_cc_script}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Generating compile_commands.json in the project root"
        VERBATIM
        USES_TERMINAL)
else()
    message(STATUS "vcvars64.bat or ninja not found - 'compile-commands' target unavailable")
endif()

# clang-tidy over src/*.cpp. Checks live in .clang-tidy; compile flags come
# from compile_commands.json in this directory:
#
#     cmake --build build --target tidy
file(GLOB WINAPP_TIDY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
find_program(CLANG_TIDY_EXE NAMES clang-tidy HINTS "$ENV{ProgramFiles}/LLVM/bin")
if(NOT CLANG_TIDY_EXE)
    file(GLOB _vs_clang_tidy
        "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang-tidy.exe")
    if(_vs_clang_tidy)
        list(GET _vs_clang_tidy 0 CLANG_TIDY_EXE)
    endif()
endif()

if(CLANG_TIDY_EXE)
    add_custom_target(tidy
        COMMAND "${CLANG_TIDY_EXE}" -p "${CMAKE_CURRENT_SOURCE_DIR}" --quiet ${WINAPP_TIDY_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "clang-tidy (checks: .clang-tidy, flags: compile_commands.json)"
        VERBATIM
        USES_TERMINAL)
    if(TARGET compile-commands)
        add_dependencies(tidy compile-commands)
    endif()
    message(STATUS "clang-tidy: ${CLANG_TIDY_EXE}")
else()
    message(STATUS "clang-tidy not found - 'tidy' target unavailable")
endif()
