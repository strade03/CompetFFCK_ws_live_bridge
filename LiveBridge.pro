QT += core gui widgets sql websockets network

CONFIG += c++17

TARGET = LiveBridge
TEMPLATE = app

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    livebridge.cpp

HEADERS += \
    mainwindow.h \
    livebridge.h

FORMS += \
    mainwindow.ui
