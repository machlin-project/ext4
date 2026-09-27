PYTHON ?= python3
MESON ?= meson
BUILD_DIR ?= .build
MESON_OPTIONS ?=
BUILD_JOBS ?= 4
TEST_JOBS ?= 2

ifeq ($(shell uname -s),Darwin)
ifeq ($(origin CC),default)
CC := xcrun --sdk macosx clang
endif
export CC
endif

.PHONY: all configure build test fskit kext format check-style clean

all: build

configure:
	$(MESON) setup --reconfigure "$(BUILD_DIR)" $(MESON_OPTIONS)

build: configure
	$(MESON) compile -C "$(BUILD_DIR)" -j $(BUILD_JOBS)

test: build
	env -i PATH="$(PATH)" $(MESON) test -C "$(BUILD_DIR)" --no-rebuild -j $(TEST_JOBS) --print-errorlogs

fskit: configure
	$(MESON) compile -C "$(BUILD_DIR)" fskit

kext: configure
	$(MESON) compile -C "$(BUILD_DIR)" kext

format:
	$(PYTHON) scripts/format.py

check-style:
	$(PYTHON) scripts/format.py --check

clean:
	$(MESON) compile -C "$(BUILD_DIR)" --clean
