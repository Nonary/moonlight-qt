QT += qml network testlib
CONFIG += testcase c++17
TARGET = tst_profiles
INCLUDEPATH += ../../app
isEmpty(MOONLIGHT_COMMON_C_INCLUDE): MOONLIGHT_COMMON_C_INCLUDE = $$PWD/../../moonlight-common-c/moonlight-common-c/src
INCLUDEPATH += $$MOONLIGHT_COMMON_C_INCLUDE
SOURCES += tst_profiles.cpp \
    ../../app/backend/profilemanager.cpp \
    ../../app/backend/nvhttp.cpp \
    ../../app/backend/nvaddress.cpp \
    ../../app/settings/streamingpreferences.cpp \
    ../../app/streaming/vrrratepolicy.cpp \
    ../../app/diagnostics/diagnosticcapture.cpp \
    ../../app/diagnostics/diagnosticzip.cpp
HEADERS += ../../app/backend/profilemanager.h ../../app/backend/nvhttp.h ../../app/settings/streamingpreferences.h
