DEFAULT_TIMING_TEST_DIRECTORY ?= .
endtoend-overlay-test: $(DEFAULT_TIMING_TEST_DIRECTORY)/tst_endtoenddeckoverlay.cpp $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(INCPATH) -fno-access-control -c $< -o endtoend-overlay-test.o
	objcopy --redefine-sym main=original_moonlight_main release/main.o endtoend-overlay-app-main.o
	$(LINK) $(LFLAGS) -Wl,--wrap=LiGetCaptureClockOffset -o $@ endtoend-overlay-test.o endtoend-overlay-app-main.o $(filter-out release/main.o,$(OBJECTS)) $(LIBS)
