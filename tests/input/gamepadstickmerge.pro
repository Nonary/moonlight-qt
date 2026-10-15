QT -= gui core
TEMPLATE = app
TARGET = tst_gamepadstickmerge
CONFIG += console testcase c++17
CONFIG -= app_bundle
SOURCES += $$PWD/tst_gamepadstickmerge.cpp
HEADERS += $$PWD/../../app/streaming/input/gamepadstickmerge.h
