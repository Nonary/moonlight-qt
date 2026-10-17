TEMPLATE = app
TARGET = tst_rollingquantile
QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle
SOURCES += $$PWD/tst_rollingquantile.cpp
HEADERS += $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/rollingquantile.h
