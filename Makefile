CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -fpermissive -Iinclude
LDFLAGS ?=
SRC := $(wildcard src/*.cpp)
TOOLS := build/nova-fmt build/nova-repl build/nova-lsp build/nova-new build/nova-check
VERSION ?= $(shell git describe --tags --always)
RELEASE_TARGET ?= linux-x86_64

all: $(TOOLS) test

test: build/tests build/nova-new build/nova-check
	./build/tests

build/tests: $(SRC) tests/parser_tests.cpp | build build/nova-check
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

build/nova-fmt: $(SRC) tools/nova_fmt.cpp | build
	$(CXX) $(CXXFLAGS) $(SRC) tools/nova_fmt.cpp -o $@ $(LDFLAGS)

build/nova-repl: $(SRC) tools/nova_repl.cpp | build
	$(CXX) $(CXXFLAGS) $(SRC) tools/nova_repl.cpp -o $@ $(LDFLAGS)

build/nova-lsp: $(SRC) tools/nova_lsp.cpp | build
	$(CXX) $(CXXFLAGS) $(SRC) tools/nova_lsp.cpp -o $@ $(LDFLAGS)

build/nova-new: $(SRC) tools/nova_new.cpp | build
	$(CXX) $(CXXFLAGS) $(SRC) tools/nova_new.cpp -o $@ $(LDFLAGS)

build/nova-check: $(SRC) tools/nova_check.cpp | build
	$(CXX) $(CXXFLAGS) $(SRC) tools/nova_check.cpp -o $@ $(LDFLAGS)

build:
	mkdir -p build

release:
	./scripts/build_release.sh --target $(RELEASE_TARGET) $(VERSION)

clean:
	rm -rf build

.PHONY: all clean test release
