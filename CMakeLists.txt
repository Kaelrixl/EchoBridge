cmake_minimum_required(VERSION 3.24)
project(EchoBridge LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "EchoBridge Native requires MSVC and the Windows SDK")
endif()

add_executable(EchoBridge WIN32 src/main.cpp resources/EchoBridge.rc)
target_compile_features(EchoBridge PRIVATE cxx_std_20)
target_compile_definitions(EchoBridge PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(EchoBridge PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(EchoBridge PRIVATE windowsapp shell32 shcore user32 gdi32 gdiplus dwmapi ole32)
target_include_directories(EchoBridge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The .rc already embeds the app manifest; prevent the linker from generating a second one.
target_link_options(EchoBridge PRIVATE /MANIFEST:NO)

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
target_include_directories(EchoBridge PRIVATE "${WINRT_INCLUDE}")

set_target_properties(EchoBridge PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    OUTPUT_NAME EchoBridge
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
