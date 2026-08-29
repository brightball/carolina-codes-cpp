CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CPPFLAGS += -Ivendor
CPPFLAGS += $(shell pkg-config --cflags libpq 2>/dev/null)
LDLIBS += $(shell pkg-config --libs libpq 2>/dev/null)
ifeq ($(strip $(LDLIBS)),)
  LDLIBS += -lpq
endif
LDLIBS += -pthread

.PHONY: all clean

all: api

api: src/main.cpp vendor/httplib.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ src/main.cpp $(LDLIBS)

clean:
	rm -f api
