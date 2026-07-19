SHELL := /bin/bash

CXX ?= g++
ZIG ?= zig
BUILD_DIR := build
VERSION := $(shell tr -d '\r\n' < VERSION)
OPENVR_DIR := $(BUILD_DIR)/openvr
OPENVR_INCLUDE_DIR := $(OPENVR_DIR)/include
OPENVR_HEADER := $(OPENVR_INCLUDE_DIR)/openvr_driver.h
NATIVE_OBJ_DIR := $(BUILD_DIR)/native
WINDOWS_OBJ_DIR := $(BUILD_DIR)/windows
TEST_DIR := $(BUILD_DIR)/tests
OVERLAY_ROOT := $(BUILD_DIR)/overlay
DIST_DIR := dist
INTEGRATION_ROOT := $(BUILD_DIR)/integration/Standable Full Body Estimation
NATIVE_SO := $(NATIVE_OBJ_DIR)/driver_standable.so
WINDOWS_HELPER := $(WINDOWS_OBJ_DIR)/standable_bridge_host.exe
STEAM_API_BRIDGE := $(WINDOWS_OBJ_DIR)/steam_api64.dll
FACTORY_TEST := $(TEST_DIR)/factory_smoke
TRANSPORT_TEST := $(TEST_DIR)/protocol_transport
RELAY_TEST := $(TEST_DIR)/provider_relay_smoke
OVERLAY_ZIP := $(BUILD_DIR)/Standable-Linux-Bridge-Overlay.zip
SOURCE_ZIP := $(BUILD_DIR)/Standable-Linux-Bridge-Source-v$(VERSION).zip

OPENVR_COMMIT := 0924064316de3effbcd1acf1e309182a2deb1c05
OPENVR_HEADER_SHA256 := 1036efe998d63e82d1d3db2b32a2f58df4a8eeaf5280f50aaf28220ff60a40ab
OPENVR_HEADER_URL := https://raw.githubusercontent.com/ValveSoftware/openvr/$(OPENVR_COMMIT)/headers/openvr_driver.h

CPPFLAGS := -Iinclude -isystem $(OPENVR_INCLUDE_DIR) -DSTANDABLE_BRIDGE_VERSION=\"$(VERSION)\"
NATIVE_CXXFLAGS := -std=c++20 -O2 -g -fPIC -fvisibility=hidden -Wall -Wextra -Wpedantic -Wconversion -Wshadow
NATIVE_LDFLAGS := -shared -Wl,--version-script=packaging/driver.exports.map -Wl,-z,defs -Wl,-z,now -Wl,-z,relro -Wl,--no-undefined
WINDOWS_CXXFLAGS := -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Wpedantic
WINDOWS_LDFLAGS := -target x86_64-windows-gnu -static -O2 -s

NATIVE_SOURCES := \
	src/native/driver_main.cpp \
	src/native/native_transport.cpp \
	src/native/relay_tracker.cpp
NATIVE_OBJECTS := $(patsubst src/native/%.cpp,$(NATIVE_OBJ_DIR)/%.o,$(NATIVE_SOURCES))
WINDOWS_SOURCES := \
	src/windows/windows_transport.cpp \
	src/windows/fake_openvr.cpp \
	src/windows/bridge_host_main.cpp
WINDOWS_OBJECTS := $(patsubst src/windows/%.cpp,$(WINDOWS_OBJ_DIR)/%.obj,$(WINDOWS_SOURCES))
STEAM_API_OBJECT := $(WINDOWS_OBJ_DIR)/steam_api_bridge.obj

.PHONY: all native windows test overlay verify integration package package-source release dist clean

all: overlay test verify

native: $(NATIVE_SO)

windows: $(WINDOWS_HELPER) $(STEAM_API_BRIDGE)

$(OPENVR_HEADER):
	@mkdir -p $(OPENVR_INCLUDE_DIR)
	curl --fail --location --silent --show-error --output $@ $(OPENVR_HEADER_URL)
	@printf '%s  %s\n' '$(OPENVR_HEADER_SHA256)' '$@' | sha256sum --check --status || { rm -f '$@'; echo 'OpenVR header checksum mismatch' >&2; exit 1; }

$(NATIVE_OBJ_DIR)/%.o: src/native/%.cpp $(OPENVR_HEADER) include/bridge_protocol.hpp
	@mkdir -p $(NATIVE_OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(NATIVE_CXXFLAGS) -MMD -MP -c $< -o $@

$(NATIVE_SO): $(NATIVE_OBJECTS) packaging/driver.exports.map
	$(CXX) $(NATIVE_LDFLAGS) $(NATIVE_OBJECTS) -ldl -o $@

$(WINDOWS_OBJ_DIR)/%.obj: src/windows/%.cpp $(OPENVR_HEADER) include/bridge_protocol.hpp
	@mkdir -p $(WINDOWS_OBJ_DIR)
	$(ZIG) c++ $(WINDOWS_CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(WINDOWS_HELPER): $(WINDOWS_OBJECTS)
	$(ZIG) c++ $(WINDOWS_LDFLAGS) $^ -lws2_32 -o $@

$(STEAM_API_OBJECT): src/windows/steam_api_bridge.cpp
	@mkdir -p $(WINDOWS_OBJ_DIR)
	$(ZIG) c++ $(WINDOWS_CXXFLAGS) -c $< -o $@

$(STEAM_API_BRIDGE): $(STEAM_API_OBJECT) packaging/steam_api64.def
	$(ZIG) c++ $(WINDOWS_LDFLAGS) -shared $^ -ladvapi32 -o $@

$(FACTORY_TEST): tests/factory_smoke.cpp
	@mkdir -p $(TEST_DIR)
	$(CXX) -std=c++20 -O2 -g -Wall -Wextra -Wpedantic $< -ldl -o $@

$(TRANSPORT_TEST): tests/protocol_transport.cpp src/native/native_transport.cpp include/bridge_protocol.hpp src/native/native_transport.hpp
	@mkdir -p $(TEST_DIR)
	$(CXX) -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Iinclude -Isrc/native tests/protocol_transport.cpp src/native/native_transport.cpp -o $@

$(RELAY_TEST): tests/provider_relay_smoke.cpp $(OPENVR_HEADER) include/bridge_protocol.hpp
	@mkdir -p $(TEST_DIR)
	$(CXX) -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Iinclude -isystem $(OPENVR_INCLUDE_DIR) $< -ldl -o $@

test: overlay $(FACTORY_TEST) $(TRANSPORT_TEST) $(RELAY_TEST)
	$(FACTORY_TEST) $(NATIVE_SO)
	$(TRANSPORT_TEST)
	$(RELAY_TEST) $(NATIVE_SO)
	bash tests/script_runtime.sh '$(OVERLAY_ROOT)'

overlay: $(NATIVE_SO) $(WINDOWS_HELPER) $(STEAM_API_BRIDGE)
	@rm -rf '$(OVERLAY_ROOT)'
	@install -d '$(OVERLAY_ROOT)/bin/linux64' '$(OVERLAY_ROOT)/bin/win64' '$(OVERLAY_ROOT)/scripts'
	@install -m 0755 '$(NATIVE_SO)' '$(OVERLAY_ROOT)/bin/linux64/driver_standable.so'
	@strip --strip-unneeded '$(OVERLAY_ROOT)/bin/linux64/driver_standable.so'
	@install -m 0755 '$(WINDOWS_HELPER)' '$(OVERLAY_ROOT)/bin/win64/standable_bridge_host.exe'
	@install -m 0644 '$(STEAM_API_BRIDGE)' '$(OVERLAY_ROOT)/bin/win64/steam_api64.dll'
	@install -m 0755 scripts/*.sh '$(OVERLAY_ROOT)/scripts/'
	@install -m 0755 install.sh '$(OVERLAY_ROOT)/scripts/bridge-installer.sh'
	@install -m 0644 README-LINUX.md '$(OVERLAY_ROOT)/README-LINUX.md'
	@install -m 0644 VERSION '$(OVERLAY_ROOT)/VERSION'

verify: overlay
	@bash scripts/verify-artifacts.sh '$(OVERLAY_ROOT)'

integration: overlay
	@test -n '$(ORIGINAL_ROOT)' || { echo 'Set ORIGINAL_ROOT to the extracted original Standable folder.' >&2; exit 1; }
	@test -f '$(ORIGINAL_ROOT)/bin/win64/driver_standable.dll' || { echo 'ORIGINAL_ROOT does not contain the original driver.' >&2; exit 1; }
	@rm -rf '$(INTEGRATION_ROOT)'
	@install -d '$(INTEGRATION_ROOT)'
	@cp -a '$(ORIGINAL_ROOT)/.' '$(INTEGRATION_ROOT)/'
	@cp -a '$(OVERLAY_ROOT)/.' '$(INTEGRATION_ROOT)/'
	@bash scripts/verify-artifacts.sh '$(INTEGRATION_ROOT)' --integrated

package: overlay verify
	@rm -f '$(OVERLAY_ZIP)'
	@(cd '$(OVERLAY_ROOT)' && zip -q -r '../$(notdir $(OVERLAY_ZIP))' .)
	@echo 'Created $(OVERLAY_ZIP)'

package-source:
	@rm -f '$(SOURCE_ZIP)'
	@zip -q -r '$(SOURCE_ZIP)' .github .gitattributes .gitignore Makefile VERSION README.md README-LINUX.md install.sh include src tests scripts docs packaging
	@echo 'Created $(SOURCE_ZIP)'

release: package package-source

dist: release
	@install -d '$(DIST_DIR)'
	@install -m 0644 '$(OVERLAY_ZIP)' '$(DIST_DIR)/Standable-Linux-Bridge-Overlay.zip'
	@(cd '$(DIST_DIR)' && sha256sum Standable-Linux-Bridge-Overlay.zip > SHA256SUMS)
	@echo 'Updated $(DIST_DIR) fallback distribution'

clean:
	rm -rf '$(BUILD_DIR)'

-include $(NATIVE_OBJECTS:.o=.d)
