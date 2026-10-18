TEMPLATE = app
TARGET = tst_plvkoffscreen
CONFIG += console c++17 link_pkgconfig
CONFIG -= qt app_bundle
PKGCONFIG += libplacebo
SOURCES += $$PWD/tst_plvkoffscreen.cpp
HEADERS += $$PWD/../../app/streaming/video/ffmpeg-renderers/plvkswapchain.h
