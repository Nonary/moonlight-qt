QT -= gui core
TEMPLATE = app
TARGET = tst_cursormodepolicy
CONFIG += console testcase c++17
CONFIG -= app_bundle
SOURCES += $$PWD/tst_cursormodepolicy.cpp
HEADERS += $$PWD/../../app/streaming/input/cursormodepolicy.h
