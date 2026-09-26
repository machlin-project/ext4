PYTHON ?= python3
BUILD_DIR ?= .build

.PHONY: all configure build test fskit kext format check-style clean

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Debug

build: configure
	cmake --build $(BUILD_DIR) --parallel 4

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

fskit:
	$(PYTHON) scripts/build_fskit.py

kext:
	$(PYTHON) scripts/build_kext.py

format:
	$(PYTHON) scripts/format.py

check-style:
	$(PYTHON) scripts/format.py --check

clean:
	cmake --build $(BUILD_DIR) --target clean
