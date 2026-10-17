include(pacingworker.pro)
TARGET = vrrworkerreplay
DEFINES += VRR_WORKER_REPLAY
SOURCES -= $$PWD/tst_vrrpacingworker.cpp
SOURCES += $$PWD/vrrworkerreplay.cpp $$PWD/vrrreplayconfig.cpp
