# lwftp 的 Qt 界面。用 Qt 5.14 打开本工程。
# 在本目录执行：qmake lwftp_qt.pro && make
# 控制连接和数据连接都使用 QTcpSocket，窗口可以下载一个远程文件。

QT += core gui widgets network
CONFIG += c++11
TEMPLATE = app
TARGET = lwftp_qt

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    lwftp_qt.cpp

HEADERS += \
    mainwindow.h \
    lwftp_qt.h

# MSVC 的 Qt 5.14 要把源文件当 UTF-8，否则中文界面和注释会乱码。
msvc: QMAKE_CXXFLAGS += /utf-8

equals(QT_MAJOR_VERSION, 5) {
    lessThan(QT_MINOR_VERSION, 14): error("需要 Qt 5.14 或更高的 Qt 5")
} else {
    error("本工程按 Qt 5.14 编写，请换用 Qt 5 的 kit")
}
