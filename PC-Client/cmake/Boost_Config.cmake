# * Конфигурация директории Boost Framework

# ! Если есть собранный буст с системной переменной (переменной среды)
# * Boost_DIR = D:\dev\Frameworks\boost_1_90_0\stage\lib\cmake\Boost-1.90.0

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

find_package(Boost 1.90.0 REQUIRED COMPONENTS headers)

# ! Если boost не собран

#include_directories(TARGET_NAME PRIVATE
#    D:/dev/Frameworks/boost_1_90_0
#)
#
#link_directories(TARGET_NAME PRIVATE
#    D:/dev/Frameworks/boost_1_90_0/libs
#)

add_compile_definitions(_WIN32_WINNT=0x0A00)