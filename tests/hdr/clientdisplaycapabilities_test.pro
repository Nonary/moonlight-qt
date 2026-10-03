TEMPLATE = app
TARGET = tst_clientdisplaycapabilities
QT += testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle

SOURCES += \
    $$PWD/tst_clientdisplaycapabilities.cpp \
    $$PWD/../../app/backend/clientdisplaycapabilities.cpp

INCLUDEPATH += $$PWD/../../app
