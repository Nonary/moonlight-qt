TEMPLATE = app
TARGET = tst_receivedriftclock
CONFIG += console c++17
CONFIG -= app_bundle qt
SOURCES += $$PWD/tst_receivedriftclock.cpp
HEADERS += $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/receivedriftclock.h
