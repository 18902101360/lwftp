# 可选的 CMake 入口，对应 Qt 5.14。日常用 lwftp_qt.pro 即可。
cmake_minimum_required(VERSION 3.5)
project(lwftp_qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt5 5.14 REQUIRED COMPONENTS Widgets Network)

add_executable(lwftp_qt
    main.cpp
    mainwindow.cpp
    lwftp_qt.cpp
    mainwindow.h
    lwftp_qt.h
)

target_link_libraries(lwftp_qt Qt5::Widgets Qt5::Network)
