# Keep the platform-neutral timing test separate from the Qt policy test: the
# former deliberately has no Qt event-loop/runtime dependency.
TEMPLATE = subdirs
CONFIG += ordered

dxgipresent.file = $$PWD/dxgipresent.pro
SUBDIRS += dxgipresent

presentationfeedback.file = $$PWD/presentationfeedback.pro
SUBDIRS += presentationfeedback

incomingtiming.file = $$PWD/incomingtiming.pro
SUBDIRS += incomingtiming
linux:packagesExist(vulkan) {
    vulkantiming.file = $$PWD/vulkantiming.pro
    SUBDIRS += vulkantiming
}
unix:!macx:packagesExist(wayland-server sdl2) {
    waylandfeedback.file = $$PWD/waylandfeedback.pro
    SUBDIRS += waylandfeedback
}

timingcontroller.file = $$PWD/timingcontroller.pro
ratepolicy.file = $$PWD/ratepolicy.pro
pacingworker.file = $$PWD/pacingworker.pro
replay.file = $$PWD/replay.pro
replayconfig.file = $$PWD/replayconfig.pro
queuesim.file = $$PWD/queuesim.pro

SUBDIRS += \
    timingcontroller \
    ratepolicy \
    pacingworker \
    replay \
    replayconfig \
    queuesim

overlay.file = $$PWD/overlay.pro
SUBDIRS += overlay
profile.file = $$PWD/profile.pro
SUBDIRS += profile
