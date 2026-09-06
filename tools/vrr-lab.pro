TEMPLATE = app
TARGET = vrr-lab
CONFIG += console c++17
CONFIG -= app_bundle qt
INCLUDEPATH += ../app/streaming/video/ffmpeg-renderers/vrr
SOURCES += vrr-lab.cpp \
    ../app/streaming/video/ffmpeg-renderers/vrr/timing.cpp \
    ../app/streaming/video/ffmpeg-renderers/vrr/trace.cpp \
    ../app/streaming/video/ffmpeg-renderers/vrr/config.cpp
