CXXFLAGS := -std=c++17 -Wall -Wextra -g -O2 -Isrc
BUILD := build
PREFIX ?= $(HOME)/.local

# Homebrew's llvm is keg-only (not on PATH), so fall back to its full path.
CLANG_TIDY ?= $(firstword $(shell command -v clang-tidy 2>/dev/null) /opt/homebrew/opt/llvm/bin/clang-tidy)
# Homebrew clang-tidy doesn't find the macOS SDK headers on its own.
SDK_PATH := $(shell xcrun --show-sdk-path 2>/dev/null)

# Every src/*.cpp is part of the engine and linked into each program.
LIB_SRCS := $(wildcard src/*.cpp)
LIB_HEADERS := $(wildcard src/*.h)
TEST_SRCS := $(wildcard tests/test_*.cpp)
TESTS := $(patsubst tests/%.cpp,$(BUILD)/%,$(TEST_SRCS))

all: $(BUILD)/xdbm $(TESTS)

$(BUILD)/xdbm: cli/main.cpp $(LIB_SRCS) $(LIB_HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ cli/main.cpp $(LIB_SRCS)

$(BUILD)/test_%: tests/test_%.cpp tests/test_util.h $(LIB_SRCS) $(LIB_HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIB_SRCS)

$(BUILD):
	mkdir -p $@

test: $(TESTS) $(BUILD)/xdbm
	@for t in $(TESTS); do ./$$t || exit 1; done
	@sh tests/cli_test.sh $(BUILD)/xdbm

# Lints every .cpp by default; `make lint LINT_FILES="a.cpp b.cpp"` lints a
# subset (the pre-commit hook uses this for staged files).
LINT_FILES ?= $(LIB_SRCS) cli/main.cpp $(TEST_SRCS)

lint:
	$(CLANG_TIDY) --quiet $(LINT_FILES) -- \
		-std=c++17 -Isrc $(if $(SDK_PATH),-isysroot $(SDK_PATH))

# Points git at the versioned hooks in .githooks/ (run once per clone).
hooks:
	git config core.hooksPath .githooks

# `make -s print-CLANG_TIDY` prints a variable's value (used by the hook).
print-%:
	@echo '$($*)'

install: $(BUILD)/xdbm
	install -d $(PREFIX)/bin
	install -m 755 $(BUILD)/xdbm $(PREFIX)/bin/xdbm

uninstall:
	rm -f $(PREFIX)/bin/xdbm

clean:
	rm -rf $(BUILD)

.PHONY: all test lint hooks install uninstall clean
