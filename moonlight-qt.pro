TEMPLATE = subdirs
SUBDIRS = \
    moonlight-common-c \
    qmdnsengine \
    app

# Build the dependencies in parallel before the final app
app.depends = qmdnsengine moonlight-common-c
win32:!winrt {
    SUBDIRS += AntiHooking
    app.depends += AntiHooking
}

!disable-h264bitstream {
    SUBDIRS += h264bitstream
    app.depends += h264bitstream
}

# PyroWave codec library (see pyrowave/VENDOR.txt). Must match the condition
# in app/app.pro. Linux uses libplacebo on x86_64 and arm64 (Granite has SSE and
# NEON math backends); macOS shares MoltenVK outputs with Metal.
win32:!winrt:contains(QT_ARCH, x86_64):!disable-pyrowave {
    SUBDIRS += pyrowave
    app.depends += pyrowave
}
linux:if(contains(QT_ARCH, x86_64)|contains(QT_ARCH, arm64)):!disable-pyrowave:!disable-libplacebo:packagesExist(libplacebo) {
    SUBDIRS += pyrowave
    app.depends += pyrowave
}
macx:!disable-prebuilts:!disable-pyrowave {
    SUBDIRS += pyrowave
    app.depends += pyrowave
}

# Support debug and release builds from command line for CI
CONFIG += debug_and_release

# Deterministic VRR tests are deliberately opt-in. Package and normal
# application builds keep their existing target set unless CONFIG+=tests is
# supplied to qmake.
contains(CONFIG, tests) {
    SUBDIRS += tests
}

# Run our compile tests
load(configure)
qtCompileTest(SL)
qtCompileTest(EGL)
