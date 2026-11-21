cmake_minimum_required(VERSION 3.20)
project(nah2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NAH2_LIBS
    d3d11
    dxgi
    d2d1
    dwrite
    dcomp
    dwmapi
    ws2_32
    user32
    gdi32
    winmm
)

add_library(nah2_core STATIC
    src/aim_assist.cpp
    src/control_panel.cpp
    src/ipc_receiver.cpp
    src/renderer.cpp
)

target_include_directories(nah2_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nah2_core PUBLIC ${NAH2_LIBS})
target_compile_definitions(nah2_core PUBLIC
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(nah2_core PUBLIC /W4 /utf-8)
else()
    target_compile_options(nah2_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

add_executable(nah2 src/nah2.cpp)
target_link_libraries(nah2 PRIVATE nah2_core)

enable_testing()
add_executable(test_nah2 tests/test_nah2.cpp)
target_link_libraries(test_nah2 PRIVATE nah2_core)
add_test(NAME nah2_tests COMMAND test_nah2)
