include(timingcontroller.pro)
TARGET = vrrbuffersim
SOURCES -= $$PWD/tst_vrrtimingcontroller.cpp
SOURCES += $$PWD/vrrbuffersim.cpp \
           $$PWD/vrrstreamsimulator.cpp \
           $$PWD/vrrstreamscenarios.cpp
HEADERS += $$PWD/vrrstreamsimulator.h $$PWD/vrrstreamscenarios.h $$PWD/vrrproductionsession.h
