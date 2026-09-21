QT -= gui core
TEMPLATE = app
TARGET = tst_dualsensehaptics
CONFIG += console c++17
CONFIG -= app_bundle
DEFINES += MOONLIGHT_HAPTICS_TEST SDL_MAIN_HANDLED
win32 {
    contains(QT_ARCH, arm64) {
        SDL_ARCH = arm64
    } else {
        SDL_ARCH = x64
    }
    INCLUDEPATH += ../../libs/windows/include/$$SDL_ARCH/SDL2
    LIBS += -L$$PWD/../../libs/windows/lib/$$SDL_ARCH -lSDL2 -lhid
} else:macx {
    INCLUDEPATH += ../../libs/mac/include/SDL2
    LIBS += -L$$PWD/../../libs/mac/lib -lSDL2
    QMAKE_RPATHDIR += $$PWD/../../libs/mac/lib
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl2
    LIBS += -pthread
}
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += tst_dualsensehaptics.cpp ../../app/streaming/input/dualsensehid.cpp
