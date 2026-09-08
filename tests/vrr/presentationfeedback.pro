TEMPLATE = app
TARGET = tst_vrrpresentationfeedback
CONFIG += console c++17
CONFIG -= qt app_bundle
SOURCES += $$PWD/tst_vrrpresentationfeedback.cpp
HEADERS += \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/prediction.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/smoothnessfeedback.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/reserve.h
