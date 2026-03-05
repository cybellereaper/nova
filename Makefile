CXX ?= g++
AR ?= ar
CPPFLAGS ?= -Iinclude
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?=

# The codebase is still transitioning from C to idiomatic C++.
# Keep permissive mode opt-in for compatibility builds.
NOVA_COMPAT ?= 1
ifeq ($(NOVA_COMPAT),1)
CXXFLAGS += -fpermissive
endif

DEPFLAGS := -MMD -MP
SRC := $(wildcard src/*.cpp)
OBJ := $(patsubst src/%.cpp,build/obj/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
TOOLS := nova-fmt nova-repl nova-lsp nova-new nova-check
VERSION ?= $(shell git describe --tags --always)
RELEASE_TARGET ?= linux-x86_64

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

all: build/tests $(addprefix build/,$(TOOLS))

build/tests: build/libnova.a tests/parser_tests.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/parser_tests.cpp build/libnova.a $(LDFLAGS) $(LDLIBS) -o $@

build/nova-fmt: build/libnova.a tools/nova_fmt.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tools/nova_fmt.cpp build/libnova.a $(LDFLAGS) $(LDLIBS) -o $@

build/nova-repl: build/libnova.a tools/nova_repl.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tools/nova_repl.cpp build/libnova.a $(LDFLAGS) $(LDLIBS) -o $@

build/nova-lsp: build/libnova.a tools/nova_lsp.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tools/nova_lsp.cpp build/libnova.a $(LDFLAGS) $(LDLIBS) -o $@

build/nova-new: build/libnova.a tools/nova_new.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tools/nova_new.cpp build/libnova.a $(LDFLAGS) $(LDLIBS) -o $@

build/nova-check: build/libnova.a tools/nova_check.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tools/nova_check.cpp build/libnova.a $(LDFLAGS) $(LDLIBS) -o $@

build/libnova.a: $(OBJ) | build
	$(AR) rcs $@ $(OBJ)

build/obj/%.o: src/%.cpp | build/obj
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

build:
	mkdir -p build

build/obj:
	mkdir -p build/obj

release:
	./scripts/build_release.sh --target $(RELEASE_TARGET) $(VERSION)

strict:
	$(MAKE) clean
	$(MAKE) NOVA_COMPAT=0 all

clean:
	rm -rf build

.PHONY: all clean release strict

-include $(DEP)
