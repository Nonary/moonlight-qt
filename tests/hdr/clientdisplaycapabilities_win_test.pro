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
            -lSDL2 dxgi.lib d3d11.lib gdi32.lib user32.lib advapi32.lib
}
