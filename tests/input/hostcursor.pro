QT -= gui core
TEMPLATE = app
TARGET = tst_hostcursor
CONFIG += console testcase c++17
CONFIG -= app_bundle
SOURCES += $$PWD/tst_hostcursor.cpp
HEADERS += $$PWD/../../app/streaming/input/hostcursormath.h
