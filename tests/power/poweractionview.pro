TEMPLATE = app
TARGET = tst_poweractionview
QT = core gui qml quick quickcontrols2 testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
SOURCES += $$PWD/tst_poweractionview.cpp
DEFINES += APP_VIEW_DIRECTORY=\\\"$$PWD/../../app/gui\\\"
RESOURCES += $$PWD/poweractionview.qrc
