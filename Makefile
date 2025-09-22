CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

# Try pkg-config first; fall back to -lhidapi
HIDAPI_CFLAGS := $(shell pkg-config --cflags hidapi 2>/dev/null | sed 's|/hidapi$$||')
HIDAPI_LIBS := $(shell pkg-config --libs hidapi 2>/dev/null)

ifeq ($(HIDAPI_LIBS),)
  HIDAPI_LIBS := -lhidapi
endif

TARGET := mx-keys-backlight
SRCS := mx-keys-backlight.c

# Install and LaunchAgent configuration
PREFIX ?= $(HOME)/.mx-keys-cli
BINDIR ?= $(PREFIX)/bin
UID := $(shell id -u)
SERVICE_LABEL := com.walkerrobertben.mxkeys.alwayson
LAUNCH_AGENTS_DIR := $(HOME)/Library/LaunchAgents
APP_SUPPORT_DIR := $(HOME)/Library/Application Support/mx-keys-backlight-cli
SCRIPT_SRC := mx-keys-always-on.sh
SCRIPT_INSTALL_PATH := $(APP_SUPPORT_DIR)/mx-keys-always-on.sh
PLIST_PATH := $(LAUNCH_AGENTS_DIR)/$(SERVICE_LABEL).plist

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(HIDAPI_CFLAGS) -o $@ $(SRCS) $(HIDAPI_LIBS)

install uninstall install-alwayson-service uninstall-alwayson-service:

install:
	install -d "$(BINDIR)"
	$(CC) $(CFLAGS) $(HIDAPI_CFLAGS) -o "$(BINDIR)/$(TARGET)" $(SRCS) $(HIDAPI_LIBS)

uninstall:
	rm -f "$(BINDIR)/$(TARGET)"

install-alwayson-service: install $(SCRIPT_SRC)
	mkdir -p "$(LAUNCH_AGENTS_DIR)"
	mkdir -p "$(APP_SUPPORT_DIR)"
	install -m 0755 "$(SCRIPT_SRC)" "$(SCRIPT_INSTALL_PATH)"
	@echo "Writing LaunchAgent plist to $(PLIST_PATH)"
	@printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0">' \
	'<dict>' \
	'  <key>Label</key><string>$(SERVICE_LABEL)</string>' \
	'  <key>ProgramArguments</key>' \
	'  <array>' \
	'    <string>$(SCRIPT_INSTALL_PATH)</string>' \
	'  </array>' \
	'  <key>EnvironmentVariables</key>' \
	'  <dict>' \
	'    <key>MX_KEYS_CLI</key><string>$(BINDIR)/$(TARGET)</string>' \
	'  </dict>' \
	'  <key>RunAtLoad</key><true/>' \
	'  <key>KeepAlive</key><true/>' \
	'  <key>ProcessType</key><string>Background</string>' \
	'  <key>WorkingDirectory</key><string>$(HOME)</string>' \
	'</dict>' \
	'</plist>' > "$(PLIST_PATH)"
	@# Prefer modern bootstrap when available; fall back to load -w
	@launchctl bootout gui/$(UID) "$(PLIST_PATH)" 2>/dev/null || true
	@launchctl bootstrap gui/$(UID) "$(PLIST_PATH)" 2>/dev/null || launchctl load -w "$(PLIST_PATH)"
	@launchctl enable gui/$(UID)/"$(SERVICE_LABEL)" 2>/dev/null || true
	@launchctl kickstart -k gui/$(UID)/"$(SERVICE_LABEL)" 2>/dev/null || launchctl start "$(SERVICE_LABEL)" 2>/dev/null || true
	@echo "Service $(SERVICE_LABEL) installed and started."

uninstall-alwayson-service:
	@launchctl bootout gui/$(UID)/"$(SERVICE_LABEL)" 2>/dev/null || launchctl stop "$(SERVICE_LABEL)" 2>/dev/null || true
	@launchctl disable gui/$(UID)/"$(SERVICE_LABEL)" 2>/dev/null || true
	@launchctl unload -w "$(PLIST_PATH)" 2>/dev/null || true
	rm -f "$(PLIST_PATH)"
	rm -f "$(SCRIPT_INSTALL_PATH)"
	@echo "Service $(SERVICE_LABEL) uninstalled."
