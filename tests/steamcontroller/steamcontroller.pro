QT -= gui core
TEMPLATE = app
TARGET = tst_steamcontroller
CONFIG += console c++17
CONFIG -= app_bundle
DEFINES += MOONLIGHT_STEAMCONTROLLER_TEST
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
unix:!macx: LIBS += -pthread
SOURCES += tst_steamcontroller.cpp \
    ../../app/streaming/input/steamcontroller/steamcontrollerattributes.cpp \
    ../../app/streaming/input/steamcontroller/steamcontrollercore.cpp \
    ../../app/streaming/input/steamcontroller/steamcontrollerlink.cpp
HEADERS += ../../app/streaming/input/steamcontroller/steamcontrollerprotocol.h \
    ../../app/streaming/input/steamcontroller/steamcontrollerattributes.h \
    ../../app/streaming/input/steamcontroller/steamcontrollercore.h \
    ../../app/streaming/input/steamcontroller/steamcontrollerlink.h
