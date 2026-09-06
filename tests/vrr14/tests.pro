TEMPLATE = app
TARGET = test-vrr14
CONFIG += console c++17
CONFIG -= app_bundle qt
QMAKE_CXXFLAGS_RELEASE -= -DNDEBUG
DEFINES -= NDEBUG
INCLUDEPATH += ../../app/streaming/video/ffmpeg-renderers/vrr
SOURCES += test_timing.cpp \
    ../../app/streaming/video/ffmpeg-renderers/vrr/timing.cpp \
    ../../app/streaming/video/ffmpeg-renderers/vrr/config.cpp
