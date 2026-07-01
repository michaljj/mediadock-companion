# CMakeList.txt : CMake project for MediaDockCompanion, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("MediaDockCompanion")

add_executable (MediaDockCompanion
    "MediaDockCompanion.cpp" "MediaDockCompanion.h"
    "Config.h"
    "Logger.cpp" "Logger.h"
    "DeviceManager.cpp" "DeviceManager.h"
    "StateStore.cpp" "StateStore.h"
    "AudioMonitor.cpp" "AudioMonitor.h"
    "MediaMonitor.cpp" "MediaMonitor.h"
    "Serializer.cpp" "Serializer.h"
    "Util.cpp" "Util.h"
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET MediaDockCompanion PROPERTY CXX_STANDARD 20)
endif()

target_link_options(MediaDockCompanion PRIVATE /ENTRY:mainCRTStartup)

find_package(cppwinrt CONFIG REQUIRED)
target_link_libraries(MediaDockCompanion PRIVATE Microsoft::CppWinRT)

set_target_properties(MediaDockCompanion PROPERTIES
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(MediaDockCompanion PRIVATE
    Microsoft::CppWinRT
    ole32
    mmdevapi
    setupapi
    hid
    comctl32
)