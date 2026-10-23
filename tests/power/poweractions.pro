TEMPLATE = app
TARGET = tst_poweractions
QT = core gui network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
INCLUDEPATH += $$PWD/../../app $$PWD/../../moonlight-common-c/moonlight-common-c/src
SOURCES += $$PWD/tst_poweractions.cpp $$PWD/../../app/backend/nvhttp.cpp $$PWD/../../app/backend/nvaddress.cpp $$PWD/../../app/backend/identitymanager.cpp
HEADERS += $$PWD/../../app/backend/nvhttp.h $$PWD/../../app/backend/poweractions.h $$PWD/tlsfixture.h
win32 {
    INCLUDEPATH += $$PWD/../../libs/windows/include
    # Match the architecture-specific OpenSSL layout used by app/app.pro.
    contains(QT_ARCH, x86_64) {
        INCLUDEPATH += $$PWD/../../libs/windows/include/x64
        LIBS += -L$$PWD/../../libs/windows/lib/x64
    }
    contains(QT_ARCH, arm64) {
        INCLUDEPATH += $$PWD/../../libs/windows/include/arm64
        LIBS += -L$$PWD/../../libs/windows/lib/arm64
    }
    LIBS += -llibssl -llibcrypto
}
macx:!disable-prebuilts {
    INCLUDEPATH += $$PWD/../../libs/mac/include
    LIBS += -L$$PWD/../../libs/mac/lib -lssl.3 -lcrypto.3
}
unix:if(!macx|disable-prebuilts) {
    CONFIG += link_pkgconfig
    PKGCONFIG += openssl
}
SOURCES += $$PWD/../../app/backend/pyrowaveudpprobe.cpp
