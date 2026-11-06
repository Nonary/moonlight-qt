TEMPLATE = app
TARGET = Spatial71SettingsTests
QT = core gui qml quick quickcontrols2 testlib
CONFIG += console testcase c++17 release
CONFIG -= app_bundle debug debug_and_release
INCLUDEPATH += $$PWD/../../app
SOURCES += $$PWD/tst_spatial71settings.cpp \
    $$PWD/../../app/settings/streamingpreferences.cpp \
    $$PWD/../../app/streaming/vrrratepolicy.cpp \
    $$PWD/../../app/diagnostics/diagnosticcapture.cpp \
    $$PWD/../../app/diagnostics/diagnosticzip.cpp
HEADERS += $$PWD/../../app/settings/streamingpreferences.h \
    $$PWD/../../app/settings/vrrtimingoptions.h
DEFINES += SPATIAL_SETTINGS_SOURCE=\\\"$$PWD/../../app/gui/SettingsView.qml\\\"
