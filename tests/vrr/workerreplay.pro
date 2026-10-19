include(pacingworker.pro)
TARGET = vrrworkerreplay
DEFINES += VRR_WORKER_REPLAY
# Replay adds members to VrrPacingWorker. Sharing its objects with the normal
# worker tests links incompatible class layouts in the ordered subdirs build.
CONFIG(debug, debug|release): OBJECTS_DIR = $$OUT_PWD/workerreplay-obj/debug
else: OBJECTS_DIR = $$OUT_PWD/workerreplay-obj/release
SOURCES -= $$PWD/tst_vrrpacingworker.cpp
SOURCES += $$PWD/vrrworkerreplay.cpp $$PWD/vrrreplayconfig.cpp
