# The test tree is intentionally opt-in.  The application and package builds
# do not enter it unless their qmake invocation explicitly adds CONFIG+=tests.
TEMPLATE = subdirs
CONFIG += ordered

contains(CONFIG, tests) {
    SUBDIRS += vrr
    linux: SUBDIRS += haptics
} else {
    message(VRR tests are disabled; rerun qmake with CONFIG+=tests)
}
