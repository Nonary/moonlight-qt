TEMPLATE = app
TARGET = tst_compositionpresentpolicy
CONFIG += console c++17
CONFIG -= qt app_bundle
SOURCES += $$PWD/tst_compositionpresentpolicy.cpp
HEADERS += $$PWD/../../app/streaming/video/ffmpeg-renderers/compositionpresentpolicy.h
