TEMPLATE = app
TARGET = Spatial71SettingsTests
QT = core gui qml quick quickcontrols2 testlib
CONFIG += console testcase c++17 release
CONFIG -= app_bundle debug debug_and_release
INCLUDEPATH += $$PWD/../../app
INCLUDEPATH += $$OUT_PWD
SPATIAL_SETTINGS_FILE = $$clean_path($$PWD/../../app/gui/SettingsView.qml)
QMAKE_SUBSTITUTES += settings-test-path.h.in
SOURCES += $$PWD/tst_spatial71settings.cpp \
    $$PWD/../../app/settings/streamingpreferences.cpp \
    $$PWD/../../app/streaming/vrrratepolicy.cpp \
    $$PWD/../../app/diagnostics/diagnosticcapture.cpp \
    $$PWD/../../app/diagnostics/diagnosticzip.cpp
HEADERS += $$PWD/../../app/settings/streamingpreferences.h \
    $$PWD/../../app/settings/vrrtimingoptions.h
