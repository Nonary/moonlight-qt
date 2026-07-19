# Keep the platform-neutral timing test separate from the Qt policy test: the
# former deliberately has no Qt event-loop/runtime dependency.
TEMPLATE = subdirs
CONFIG += ordered

timingcontroller.file = $$PWD/timingcontroller.pro
ratepolicy.file = $$PWD/ratepolicy.pro
pacingworker.file = $$PWD/pacingworker.pro

SUBDIRS += \
    timingcontroller \
    ratepolicy \
    pacingworker
