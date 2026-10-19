cmake_minimum_required(VERSION 3.20)

project(TaskbarTuner VERSION 2.0.0 LANGUAGES CXX RC)

# The program is a single .exe, but it is built from two targets: a host
# executable and a DLL that gets injected into explorer.exe. The DLL is embedded
# in the executable as an RCDATA resource, so the user only ever sees one file.

if(NOT MSVC)
    message(FATAL_ERROR
        "TaskbarTuner needs the Microsoft compiler: the hook uses the Windows "
        "SDK's C++/WinRT headers and structured exception handling.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build this project for x64; the taskbar is a 64-bit process.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # /utf-8 matters: the UI strings are Chinese and the sources are UTF-8.
    add_compile_options(/utf-8 /permissive- /W4)
endif()

# ---------------------------------------------------------------------------
# Hook DLL
# ---------------------------------------------------------------------------

add_library(TaskbarTunerHook SHARED
    src/hook/dllmain.cpp
    src/hook/TaskbarHost.cpp
    src/hook/VisualTree.cpp
    src/hook/BackdropBrush.cpp
    src/shared/Config.cpp
    src/shared/Log.cpp
)

target_include_directories(TaskbarTunerHook PRIVATE src res)

# windowsapp.lib is the C++/WinRT umbrella import library: it resolves
# RoActivateInstance for every projected type without listing the individual
# Windows.*.dll forwarders.
target_link_libraries(TaskbarTunerHook PRIVATE
    windowsapp
    comctl32
    ole32
    user32
    gdi32
    advapi32
)

# Fixed output directory regardless of configuration: the resource script needs
# a path that is known at configure time.
set(TUNER_HOOK_DIR "${CMAKE_BINARY_DIR}/hook")
set_target_properties(TaskbarTunerHook PROPERTIES
    OUTPUT_NAME "TaskbarTunerHook"
    RUNTIME_OUTPUT_DIRECTORY "${TUNER_HOOK_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TUNER_HOOK_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TUNER_HOOK_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${TUNER_HOOK_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${TUNER_HOOK_DIR}"
)

# ---------------------------------------------------------------------------
# Host executable (single file: the DLL above is a resource of this one)
# ---------------------------------------------------------------------------

add_executable(TaskbarTuner WIN32
    src/host/main.cpp
    src/host/Panel.cpp
    src/host/Injector.cpp
    src/shared/Config.cpp
    src/shared/Log.cpp
)

target_include_directories(TaskbarTuner PRIVATE src res)
target_link_libraries(TaskbarTuner PRIVATE
    shell32
    comctl32
    dwmapi
    advapi32
    user32
    gdi32
)

# The resource script embeds the DLL, so it can only be compiled once the DLL
# exists. configure_file substitutes these three paths into the template.
set(TUNER_HOOK_DLL "${TUNER_HOOK_DIR}/TaskbarTunerHook.dll")
file(TO_CMAKE_PATH "${TUNER_HOOK_DLL}" TUNER_HOOK_DLL)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/TaskbarTuner.ico" TUNER_ICON)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest" TUNER_MANIFEST)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/TaskbarTuner.rc.in"
    "${CMAKE_BINARY_DIR}/generated/TaskbarTuner.rc"
    @ONLY
)

target_sources(TaskbarTuner PRIVATE "${CMAKE_BINARY_DIR}/generated/TaskbarTuner.rc")
add_dependencies(TaskbarTuner TaskbarTunerHook)

# rc.exe has its own include path; target_include_directories does not reach it.
# resource.h and version.h are #included by the generated resource script.
string(APPEND CMAKE_RC_FLAGS " /I\"${CMAKE_CURRENT_SOURCE_DIR}/res\"")
string(APPEND CMAKE_RC_FLAGS " /I\"${CMAKE_CURRENT_SOURCE_DIR}/src\"")

set_target_properties(TaskbarTuner PROPERTIES
    OUTPUT_NAME "TaskbarTuner"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
)

# A PDB in the output folder would be the only extra file; the shipped program
# is meant to be a single .exe, so no debug database is requested.
message(STATUS "TaskbarTuner: the finished program is ${CMAKE_BINARY_DIR}/bin/TaskbarTuner.exe")
