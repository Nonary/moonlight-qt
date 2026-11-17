TEMPLATE = app
TARGET = tst_udpreceive
CONFIG += console c11
CONFIG -= app_bundle
QT -= gui core
# The streaming core's internal headers use C11 atomics, which MSVC enables separately
*-msvc: QMAKE_CFLAGS += -std:c11 -experimental:c11atomics

COMMON = $$clean_path($$PWD/../../moonlight-common-c/moonlight-common-c)
INCLUDEPATH += $$COMMON/src $$COMMON/nanors $$COMMON/nanors/deps $$COMMON/nanors/deps/obl $$COMMON/enet/include

SOURCES += tst_udpreceive.c \
           $$COMMON/src/PlatformSockets.c

win32 {
    QMAKE_CFLAGS += /Gy
    QMAKE_LFLAGS += /OPT:REF
    LIBS += -lws2_32 -lwinmm
}
unix {
    QMAKE_CFLAGS += -ffunction-sections -fdata-sections
    macx: QMAKE_LFLAGS += -Wl,-dead_strip
    else: QMAKE_LFLAGS += -Wl,--gc-sections
    LIBS += -pthread
}
