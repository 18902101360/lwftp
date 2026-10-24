QT += core gui widgets network

CONFIG += c++11 console
TARGET = lwftp_gui
TEMPLATE = app

# lwftp 库（纯 C，无 lwIP 依赖）+ Qt 实现的移植层
INCLUDEPATH += .. .

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    ftpengine.cpp \
    lwftp_port_qt.cpp \
    ../lwftp.c

HEADERS += \
    mainwindow.h \
    ftpengine.h \
    lwftp_port_qt.h \
    ../lwftp_port.h
