cmake_minimum_required(VERSION 3.24)
project(EchoBridgeNative LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "EchoBridge Native requires MSVC and the Windows SDK")
endif()

add_executable(EchoBridgeNative WIN32 main.cpp EchoBridge.rc)
target_compile_features(EchoBridgeNative PRIVATE cxx_std_20)
target_compile_definitions(EchoBridgeNative PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(EchoBridgeNative PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(EchoBridgeNative PRIVATE windowsapp shell32 user32 gdi32 dwmapi ole32)

# Desktop C++/WinRT projection headers ship with the Windows SDK.
file(GLOB WINRT_HEADERS "${CMAKE_SYSTEM_PREFIX_PATH}/Include/*/cppwinrt/winrt/base.h"
    "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt/winrt/base.h")
if(NOT WINRT_HEADERS)
    message(FATAL_ERROR "Windows SDK C++/WinRT headers were not found")
endif()
list(SORT WINRT_HEADERS COMPARE NATURAL ORDER DESCENDING)
list(GET WINRT_HEADERS 0 WINRT_BASE)
get_filename_component(WINRT_DIR "${WINRT_BASE}" DIRECTORY)
get_filename_component(WINRT_INCLUDE "${WINRT_DIR}" DIRECTORY)
target_include_directories(EchoBridgeNative PRIVATE "${WINRT_INCLUDE}")

set_target_properties(EchoBridgeNative PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    OUTPUT_NAME EchoBridge
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
