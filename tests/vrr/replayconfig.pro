TEMPLATE = app
TARGET = tst_vrrreplayconfig

QT += testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle

SOURCES += \
    $$PWD/tst_vrrreplayconfig.cpp \
    $$PWD/vrrreplayconfig.cpp \
    $$PWD/vrrreplaymodel.cpp

HEADERS += \
    $$PWD/vrrreplayconfig.h \
    $$PWD/vrrreplaymodel.h

win32:contains(QT_ARCH, x86_64) {
    INCLUDEPATH += $$PWD/../../libs/windows/include/x64
}
