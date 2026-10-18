# * Конфигурация Qt5 Framework

find_package(Qt5 COMPONENTS Core Widgets SerialPort REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)