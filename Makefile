CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CPPFLAGS += -Ivendor
CPPFLAGS += $(shell pkg-config --cflags libpq 2>/dev/null)
LDLIBS += $(shell pkg-config --libs libpq 2>/dev/null)
ifeq ($(strip $(LDLIBS)),)
  LDLIBS += -lpq
endif
LDLIBS += -pthread

.PHONY: all clean test

all: api

api: src/main.cpp vendor/httplib.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ src/main.cpp $(LDLIBS)

perf_test: src/main.cpp src/perf_test.cpp src/carolina.h vendor/httplib.h
	$(CXX) $(CXXFLAGS) -Wno-unused-function -Wno-unused-variable -DCAROLINA_TEST $(CPPFLAGS) -Isrc -o $@ src/main.cpp src/perf_test.cpp $(LDLIBS)

test: perf_test
	./perf_test

clean:
	rm -f api perf_test
