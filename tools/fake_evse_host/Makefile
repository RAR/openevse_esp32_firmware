# Host-side fake OpenEVSE controller. Plain g++; no PlatformIO, no Arduino.
# The protocol brain (../../src/fake_evse_core.cpp) is shared verbatim with the
# in-firmware FAKE_EVSE build and its doctest suite -- do not fork it here.

CXX      ?= g++
CXXFLAGS ?= -std=gnu++17 -O2 -Wall -Wextra
SRCDIR   := ../../src

BIN  := fake_evse_host
SRCS := main.cpp $(SRCDIR)/fake_evse_core.cpp

all: $(BIN)

$(BIN): $(SRCS) $(SRCDIR)/fake_evse_core.h
	$(CXX) $(CXXFLAGS) -I$(SRCDIR) -o $@ $(SRCS)

clean:
	rm -f $(BIN)

.PHONY: all clean
