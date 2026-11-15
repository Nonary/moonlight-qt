TEMPLATE = app
TARGET = tst_controllernavigation
QT += gui qml quick quickcontrols2 network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
macx:!disable-prebuilts {
    INCLUDEPATH += $$PWD/../../libs/mac/include $$PWD/../../libs/mac/include/SDL2
    LIBS += -L$$PWD/../../libs/mac/lib -lSDL2
} else:win32 {
    DEFINES += SDL_MAIN_HANDLED
    INCLUDEPATH += $$PWD/../../libs/windows/include/x64 $$PWD/../../libs/windows/include/x64/SDL2
    LIBS += -L$$PWD/../../libs/windows/lib/x64 -lSDL2
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl2
}
INCLUDEPATH += $$PWD/../../app
SOURCES += $$PWD/tst_controllernavigation.cpp \
    $$PWD/../../app/gui/recentappsmodel.cpp \
    $$PWD/../../app/gui/sdlgamepadkeynavigation.cpp \
    $$PWD/../../app/settings/streamingpreferences.cpp \
    $$PWD/../../app/streaming/vrrratepolicy.cpp \
    $$PWD/../../app/diagnostics/diagnosticcapture.cpp \
    $$PWD/../../app/diagnostics/diagnosticzip.cpp
HEADERS += $$PWD/../../app/gui/sdlgamepadkeynavigation.h \
    $$PWD/../../app/gui/recentappsmodel.h \
    $$PWD/../../app/settings/streamingpreferences.h

DEFINES += CONSOLE_GUI_PATH=\\\"$$PWD/../../app/gui\\\"
DEFINES += CONSOLE_MOCKS_PATH=\\\"$$PWD/console-mocks\\\"

HEADERS += $$PWD/consolefixtures.h
RESOURCES += $$PWD/../power/poweractionview.qrc $$PWD/../../app/qml.qrc
