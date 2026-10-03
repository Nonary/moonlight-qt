TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS += capabilities
capabilities.file = $$PWD/clientdisplaycapabilities_test.pro

win32:!winrt {
    SUBDIRS += windows
    windows.file = $$PWD/clientdisplaycapabilities_win_test.pro
}
