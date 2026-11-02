# Tools
ARDUINO_CLI   ?= arduino-cli
TIO           ?= tio

# Board configuration
FQBN          ?= esp32:esp32:esp32s3
BOARD_OPTIONS ?= CDCOnBoot=cdc

# Serial port detection (prefers first connected ACM/USB serial device)
DETECTED_PORT := $(shell $(ARDUINO_CLI) board list 2>/dev/null | grep -E '^/dev/tty(ACM|USB)' | head -n1 | awk '{print $$1}')
PORT          ?= $(if $(DETECTED_PORT),$(DETECTED_PORT),/dev/ttyACM0)
BAUD          ?= 115200

# Directories and sketch
SKETCH_DIR    ?= $(CURDIR)
BUILD_DIR     ?= $(SKETCH_DIR)/build

# Files to temporarily hide during compilation (per AGENTS.md)
IGNORE_FILES  ?= Pulse_Oximeter_Plus_Google_Sheet.ino

# Optional extra flags for compiler
EXTRA_FLAGS   ?=

.PHONY: all build compile flash upload monitor clean ports boards help

all: compile

# Helper macro to run a command while hiding incompatible .ino files
define run_isolated
	@set -e; \
	hidden=""; \
	for f in $(IGNORE_FILES); do \
		if [ -f "$(SKETCH_DIR)/$$f" ]; then \
			mv "$(SKETCH_DIR)/$$f" "$(SKETCH_DIR)/.$$f.tmp_ignored"; \
			hidden="$$hidden $$f"; \
		fi; \
	done; \
	restore() { \
		for f in $$hidden; do \
			if [ -f "$(SKETCH_DIR)/.$$f.tmp_ignored" ]; then \
				mv "$(SKETCH_DIR)/.$$f.tmp_ignored" "$(SKETCH_DIR)/$$f"; \
			fi; \
		done; \
	}; \
	trap restore EXIT INT TERM; \
	$1; \
	STATUS=$$?; \
	restore; \
	trap - EXIT INT TERM; \
	exit $$STATUS
endef

compile:
	@echo "==> Compiling sketch for $(FQBN)..."
	@mkdir -p $(BUILD_DIR)
	$(call run_isolated,$(ARDUINO_CLI) compile \
		--fqbn $(FQBN) \
		--build-path $(BUILD_DIR) \
		$(if $(BOARD_OPTIONS),--board-options $(BOARD_OPTIONS),) \
		$(EXTRA_FLAGS) \
		$(SKETCH_DIR))
	@echo "==> Compilation successful. Binaries located in $(BUILD_DIR)/"

build: compile

flash: compile
	@echo "==> Flashing binary to $(PORT) using $(FQBN)..."
	$(ARDUINO_CLI) upload \
		--fqbn $(FQBN) \
		--port $(PORT) \
		--input-dir $(BUILD_DIR) \
		$(SKETCH_DIR)
	@echo "==> Flash completed successfully!"

upload: flash

monitor:
	@echo "==> Connecting serial monitor to $(PORT) at $(BAUD) baud..."
	@if command -v $(TIO) >/dev/null 2>&1; then \
		$(TIO) -b $(BAUD) $(PORT); \
	else \
		$(ARDUINO_CLI) monitor -p $(PORT) -c baudrate=$(BAUD); \
	fi

ports boards:
	@echo "==> Detecting connected boards and ports:"
	@$(ARDUINO_CLI) board list

clean:
	@echo "==> Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@for f in $(IGNORE_FILES); do \
		if [ -f "$(SKETCH_DIR)/.$$f.tmp_ignored" ]; then \
			mv "$(SKETCH_DIR)/.$$f.tmp_ignored" "$(SKETCH_DIR)/$$f"; \
		fi; \
	done
	@echo "==> Clean done."

help:
	@echo "Available targets:"
	@echo "  make               - Compile the sketch (default target)"
	@echo "  make compile       - Compile the sketch into build/"
	@echo "  make flash         - Compile and upload firmware to board"
	@echo "  make upload        - Alias for 'flash'"
	@echo "  make monitor       - Open serial monitor (using tio or arduino-cli)"
	@echo "  make ports         - List connected boards and serial ports"
	@echo "  make clean         - Remove build directory"
	@echo "  make help          - Display this help message"
	@echo ""
	@echo "Configuration variables (can override on command line):"
	@echo "  FQBN=<fqbn>        (current: $(FQBN))"
	@echo "  PORT=<device>      (current: $(PORT))"
	@echo "  BAUD=<baudrate>    (current: $(BAUD))"
	@echo "  BUILD_DIR=<path>   (current: $(BUILD_DIR))"
	@echo ""
	@echo "Example usage:"
	@echo "  make compile"
	@echo "  make flash PORT=/dev/ttyACM0"
	@echo "  make monitor"
