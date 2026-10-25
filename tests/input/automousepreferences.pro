TEMPLATE = app
TARGET = tst_automousepreferences
QT += gui qml testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
INCLUDEPATH += $$PWD/../../app
SOURCES += $$PWD/tst_automousepreferences.cpp \
    $$PWD/../../app/settings/streamingpreferences.cpp \
    $$PWD/../../app/streaming/vrrratepolicy.cpp \
    $$PWD/../../app/diagnostics/diagnosticcapture.cpp \
    $$PWD/../../app/diagnostics/diagnosticzip.cpp
HEADERS += $$PWD/../../app/settings/streamingpreferences.h
