TEMPLATE = app
TARGET = tst_metalpresenter
CONFIG += console c++17
CONFIG -= app_bundle
QT += core gui quick
DEFINES += SDL_MAIN_HANDLED

!macx:error("The native Metal presenter smoke test requires macOS")

SOURCES += \
    $$PWD/tst_metalpresenter.mm \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/vt_metal.mm \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/vt_base.mm \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/macdisplaytiming.mm \
    $$PWD/../../app/streaming/streamutils.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingtrace.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtargetwaiter.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/vrr/profile.cpp

INCLUDEPATH += \
    $$PWD/../../app \
    $$PWD/../../moonlight-common-c/moonlight-common-c/src \
    $$PWD/../../qmdnsengine/qmdnsengine/src/include \
    $$PWD/../../qmdnsengine

!disable-prebuilts {
    INCLUDEPATH += $$PWD/../../libs/mac/include \
                   $$PWD/../../libs/mac/include/SDL2
    LIBS += -L$$PWD/../../libs/mac/lib -lavutil.60 -lSDL2
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += libavutil sdl2 SDL2_ttf opus
}

LIBS += -framework Metal -framework MetalKit -framework Cocoa \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
        -framework QuartzCore -framework AVFoundation -framework ApplicationServices

# App headers are needed for the renderer's inactive overlay vtable entry.
# The smoke harness supplies only clocks, file lookup and inactive overlays;
# all drawable, rendering, source retention and presentation code is production.
HEADERS += $$PWD/assertions.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/macdisplaytiming.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/ivrrframepresenter.h \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/presentationclock.h

# Opt in to the codec/Metal integration cases explicitly; ordinary test builds
# need not compile the vendored GPU codec or open a second native smoke session.
contains(CONFIG, pyrowave) {
    DEFINES += HAVE_PYROWAVE
    nativePresenterSources = $$SOURCES
    include($$PWD/../../pyrowave/pyrowave.pri)
    !isEmpty(PYROWAVE_STATIC_LIBRARY) {
        SOURCES = $$nativePresenterSources
        LIBS += $$PYROWAVE_STATIC_LIBRARY
        PRE_TARGETDEPS += $$PYROWAVE_STATIC_LIBRARY
    }
    include($$PWD/../../pyrowave/compression/compression.pri)
    SOURCES += $$PWD/metalpyrowavefixture.cpp \
        $$PWD/../../app/streaming/video/pyrowave/pyrowavedecoder.cpp \
        $$PWD/../../app/streaming/video/pyrowave/pyrowaveframing.cpp \
        $$PWD/../../app/streaming/video/pyrowave/pyrowavemetal.mm
    HEADERS += $$PWD/metalpyrowavefixture.h \
        $$PWD/../../app/streaming/video/pyrowave/pyrowavemetal.h
}
