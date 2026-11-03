TEMPLATE = app
TARGET = Spatial71QueueTests
QT -= core gui
CONFIG += console c++17 release
CONFIG -= app_bundle debug debug_and_release
SOURCES += tst_spatial71queue.cpp
HEADERS += ../../app/streaming/audio/renderers/spatialaudioqueue.h
win32-msvc {
    QMAKE_CXXFLAGS_WARN_ON = /W4
    QMAKE_CXXFLAGS += /WX
    contains(CONFIG, spatial_analyze): QMAKE_CXXFLAGS += /analyze
} else {
    QMAKE_CXXFLAGS += -Wall -Wextra -Werror -pedantic
}
