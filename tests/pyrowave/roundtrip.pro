TEMPLATE = app
TARGET = tst_pyrowaveroundtrip
QT -= gui core
CONFIG += console c++17 warn_off
CONFIG -= app_bundle

include($$PWD/../../pyrowave/pyrowave.pri)

SOURCES += \
    $$PWD/tst_pyrowaveroundtrip.cpp \
    $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.cpp
HEADERS += $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.h

win32: LIBS += -luser32
