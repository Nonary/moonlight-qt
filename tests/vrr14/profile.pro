TEMPLATE = app
TARGET = test-vrr14-profile
QT = core
CONFIG += console c++17
CONFIG -= app_bundle
DEFINES -= NDEBUG
INCLUDEPATH += ../../app/streaming/video/ffmpeg-renderers/vrr
SOURCES += test_profile.cpp ../../app/streaming/video/ffmpeg-renderers/vrr/profile.cpp
