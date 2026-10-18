TEMPLATE = app
TARGET = tst_launchwarning
QT = core
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += $$PWD/../../app
SOURCES += $$PWD/tst_launchwarning.cpp
HEADERS += $$PWD/../../app/backend/launchwarning.h
