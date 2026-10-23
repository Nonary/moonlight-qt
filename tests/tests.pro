# The test tree is intentionally opt-in.  The application and package builds
# do not enter it unless their qmake invocation explicitly adds CONFIG+=tests.
TEMPLATE = subdirs
CONFIG += ordered

contains(CONFIG, tests) {
    powerActions.file = $$PWD/power/poweractions.pro
    powerActionView.file = $$PWD/power/poweractionview.pro
    SUBDIRS += powerActions powerActionView
    launchWarning.file = $$PWD/backend/launchwarning.pro
    SUBDIRS += launchWarning
    SUBDIRS += updates
    SUBDIRS += vrr
    SUBDIRS += haptics
    SUBDIRS += steamcontroller
    SUBDIRS += pyrowave
    toleranceInput.file = $$PWD/input/tolerance.pro
    SUBDIRS += toleranceInput
    gamepadStickMerge.file = $$PWD/input/gamepadstickmerge.pro
    SUBDIRS += gamepadStickMerge
    calibrationHosts.file = $$PWD/qml/calibration-hosts.pro
    SUBDIRS += calibrationHosts
    macx {
        controllerNavigation.file = $$PWD/qml/controller-navigation.pro
        SUBDIRS += controllerNavigation
    } else:unix:packagesExist(sdl2) {
        controllerNavigation.file = $$PWD/qml/controller-navigation.pro
        SUBDIRS += controllerNavigation
    }
} else {
    message(VRR tests are disabled; rerun qmake with CONFIG+=tests)
}
