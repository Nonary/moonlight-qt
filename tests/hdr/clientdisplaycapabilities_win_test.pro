TEMPLATE = app
TARGET = tst_clientdisplaycapabilities_win
QT += testlib gui
CONFIG += console testcase c++17
CONFIG -= app_bundle

SOURCES += \
    $$PWD/tst_clientdisplaycapabilities_win.cpp \
    $$PWD/../../app/backend/clientdisplaycapabilities.cpp \
    $$PWD/../../app/backend/clientdisplaycapabilities_win.cpp

INCLUDEPATH += $$PWD/../../app

contains(QT_ARCH, x86_64) {
    INCLUDEPATH += $$PWD/../../libs/windows/include/x64 \
                   $$PWD/../../libs/windows/include/x64/SDL2
    LIBS += -L$$PWD/../../libs/windows/lib/x64 \
            -lSDL2
}
contains(QT_ARCH, arm64) {
    INCLUDEPATH += $$PWD/../../libs/windows/include/arm64 \
                   $$PWD/../../libs/windows/include/arm64/SDL2
    LIBS += -L$$PWD/../../libs/windows/lib/arm64 -lSDL2
}

win32: LIBS += dxgi.lib d3d11.lib gdi32.lib user32.lib advapi32.lib
