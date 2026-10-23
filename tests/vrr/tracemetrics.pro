TEMPLATE = app
TARGET = tst_vrrtracemetrics
CONFIG += console c++17
CONFIG -= qt app_bundle
SOURCES += $$PWD/tst_vrrtracemetrics.cpp
HEADERS += $$PWD/vrrreplaymetrics.h \
    $$PWD/../../app/streaming/video/timinggraph.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/cadencedetection.h
