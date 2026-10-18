cmake_minimum_required(VERSION 3.21)
project(ESP32GamepadBridge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

include(Qt5_Config)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Widgets SerialPort)

add_library(bridge_core
    src/protocol.cpp
    src/profile.cpp
    src/virtual_ds4.cpp
)
target_include_directories(bridge_core PUBLIC src)
target_link_libraries(bridge_core PUBLIC Qt5::Core)

add_executable(esp32-gamepad-bridge WIN32
    src/main.cpp
    src/main_window.cpp
)
target_link_libraries(esp32-gamepad-bridge PRIVATE
    bridge_core
    Qt5::Widgets
    Qt5::SerialPort
)

if(MSVC)
    target_compile_options(bridge_core PRIVATE /W4 /permissive-)
    target_compile_options(esp32-gamepad-bridge PRIVATE /W4 /permissive-)
else()
    target_compile_options(bridge_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(esp32-gamepad-bridge PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(TARGET esp32-gamepad-bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/profiles"
        "$<TARGET_FILE_DIR:esp32-gamepad-bridge>/profiles"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(bridge-core-tests tests/core_tests.cpp)
    target_link_libraries(bridge-core-tests PRIVATE bridge_core)
    add_test(NAME bridge-core-tests COMMAND bridge-core-tests)
    set_tests_properties(bridge-core-tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
