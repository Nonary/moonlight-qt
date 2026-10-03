# The test tree is intentionally opt-in.  The application and package builds
# do not enter it unless their qmake invocation explicitly adds CONFIG+=tests.
TEMPLATE = subdirs
CONFIG += ordered

contains(CONFIG, tests) {
    SUBDIRS += vrr
    SUBDIRS += haptics
    SUBDIRS += pyrowave
    SUBDIRS += profiles
    SUBDIRS += navigation
    gameSettings.subdir = game-settings
    SUBDIRS += gameSettings
} else {
    message(VRR tests are disabled; rerun qmake with CONFIG+=tests)
}
