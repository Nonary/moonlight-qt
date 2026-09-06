TEMPLATE = app
TARGET = test-vrr14-worker
QT = core qml
CONFIG += console c++17 link_pkgconfig
CONFIG -= app_bundle
DEFINES -= NDEBUG
PKGCONFIG += sdl2 libavcodec libavutil openssl
INCLUDEPATH += ../../app ../../app/streaming/video/ffmpeg-renderers/vrr ../../moonlight-common-c/moonlight-common-c/src
SOURCES += test_worker.cpp \
    ../../app/streaming/video/ffmpeg-renderers/vrr/profile.cpp \
    ../../app/streaming/video/ffmpeg-renderers/vrr/worker.cpp \
    ../../app/streaming/video/ffmpeg-renderers/vrr/clock.cpp \
    ../../app/streaming/video/ffmpeg-renderers/vrr/timing.cpp \
    ../../app/streaming/video/ffmpeg-renderers/vrr/trace.cpp
LIBS += $$PWD/../../build-tests/vrr14/moonlight-common-c/libmoonlight-common-c.a
