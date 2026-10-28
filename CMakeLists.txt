cmake_minimum_required(VERSION 3.16)
project(Windira VERSION 1.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "Windira is a Windows-only project.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: a single portable exe with no Visual C++ redistributable to install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /MP /wd4100)
endif()

add_library(windira_core STATIC
  src/core/Common.cpp
  src/core/Devices.cpp
  src/core/Json.cpp
  src/core/Renamer.cpp
  src/core/Reset.cpp
  src/core/State.cpp
  src/core/Targets.cpp
  src/core/Task.cpp
)
target_include_directories(windira_core PUBLIC src/core)
target_link_libraries(windira_core PUBLIC setupapi cfgmgr32 ole32 shell32 advapi32 user32 wevtapi version)

add_executable(windira-cli src/cli/main.cpp src/cli/cli.rc src/cli/cli.manifest)
target_link_libraries(windira-cli PRIVATE windira_core)

add_executable(windira WIN32
  src/app/main.cpp
  src/app/MainWindow.cpp
  src/app/RenameDialog.cpp
  src/app/Preview.cpp
  src/app/app.rc
  src/app/app.manifest
)
target_link_libraries(windira PRIVATE windira_core comctl32 uxtheme dwmapi mfplat mf mfreadwrite mfuuid)

# Our .manifest files (UAC level, DPI awareness, Common Controls 6) are merged in by CMake.
# Stop the linker from adding its own default "asInvoker" UAC entry, which would win.
foreach(exe windira windira-cli)
  target_link_options(${exe} PRIVATE /MANIFESTUAC:NO)
endforeach()

enable_testing()
add_executable(windira-tests tests/core_tests.cpp)
target_link_libraries(windira-tests PRIVATE windira_core)
add_test(NAME core COMMAND windira-tests)
