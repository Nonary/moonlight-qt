TEMPLATE = app
TARGET = Spatial71RendererTest
QT = core
CONFIG += console c++17 release
CONFIG -= app_bundle debug debug_and_release
win32-msvc {
    INCLUDEPATH += ../../moonlight-common-c/moonlight-common-c/src
    INCLUDEPATH += ../../app
    INCLUDEPATH += ../../libs/windows/include/x64 ../../libs/windows/include/x64/SDL2
    LIBS += -L$$PWD/../../libs/windows/lib/x64 -lSDL2 -lole32 -luuid -lavrt -lpropsys
    DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
    QMAKE_CXXFLAGS_WARN_ON = /W4
    QMAKE_CXXFLAGS += /WX
    contains(CONFIG, spatial_analyze): QMAKE_CXXFLAGS += /analyze
    SOURCES += spatial71_renderer_test.cpp ../../app/streaming/audio/renderers/spatialaudio.cpp
    HEADERS += ../../app/streaming/audio/renderers/spatialaudio.h ../../app/streaming/audio/renderers/spatialaudioqueue.h
} else {
    error("The native renderer harness requires Windows x64 and MSVC. The queue suite is portable.")
}
