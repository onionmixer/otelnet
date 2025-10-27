# otelnet Makefile
# Standalone Telnet Client with File Transfer Support

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -Werror -O2 -std=gnu11 -D_GNU_SOURCE
LDFLAGS =
LIBS =

# ekermit feature flags
# Enhanced features enabled:
#   -DF_CRC: Enable CRC-16 block check (Type 3) for 99.998% error detection
#   -DF_AT:  Enable Attribute packets for file metadata (size, date, permissions)
#   -DNO_LP: Disable Long Packets (use standard packet size ~94 bytes)
#   -DNO_SSW: Disable Sliding Windows (use stop-and-wait mode)
KERMIT_FLAGS = -DF_CRC -DF_AT -DNO_LP -DNO_SSW

# Directories
SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
EKERMIT_DIR = ekermit

# Target executables
TARGET = otelnet
TARGET_STATIC = otelnet_static

# Source files
SOURCES = $(SRC_DIR)/otelnet.c $(SRC_DIR)/telnet.c $(SRC_DIR)/transfer.c $(SRC_DIR)/kermit_client.c
EKERMIT_SOURCES = $(EKERMIT_DIR)/kermit.c
ALL_SOURCES = $(SOURCES) $(EKERMIT_SOURCES)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES)) $(OBJ_DIR)/kermit.o
OBJECTS_STATIC = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%-static.o,$(SOURCES)) $(OBJ_DIR)/kermit-static.o

# Header dependencies
INCLUDES = -I$(INC_DIR) -I$(EKERMIT_DIR)

# Debug build option
DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -DDEBUG -O0
endif

# Default target
.PHONY: all
all: $(TARGET)

# Create directories
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

# Link otelnet
$(TARGET): $(OBJ_DIR) $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CC) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $(TARGET)
	@echo "Build complete: $(TARGET)"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $(KERMIT_FLAGS) $(INCLUDES) -c $< -o $@

# Compile ekermit (disable -Werror for third-party code)
$(OBJ_DIR)/kermit.o: $(EKERMIT_DIR)/kermit.c | $(OBJ_DIR)
	@echo "Compiling $<..."
	$(CC) $(filter-out -Werror,$(CFLAGS)) $(KERMIT_FLAGS) -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-sign-compare $(INCLUDES) -c $< -o $@

# Compile source files for static build
$(OBJ_DIR)/%-static.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@echo "Compiling $< (static)..."
	$(CC) $(CFLAGS) $(KERMIT_FLAGS) -Wno-macro-redefined $(INCLUDES) -c $< -o $@

# Compile ekermit for static build (disable -Werror for third-party code)
$(OBJ_DIR)/kermit-static.o: $(EKERMIT_DIR)/kermit.c | $(OBJ_DIR)
	@echo "Compiling $< (static)..."
	$(CC) $(filter-out -Werror,$(CFLAGS)) $(KERMIT_FLAGS) -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-sign-compare $(INCLUDES) -c $< -o $@

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(OBJ_DIR) $(TARGET) $(TARGET_STATIC)
	@echo "Clean complete"

# Install (requires root)
.PHONY: install
install: $(TARGET)
	@echo "Installing otelnet..."
	install -m 755 $(TARGET) /usr/local/bin/otelnet
	install -m 644 otelnet.conf /etc/otelnet.conf.example
	@echo "Installation complete"

# Uninstall
.PHONY: uninstall
uninstall:
	@echo "Uninstalling otelnet..."
	rm -f /usr/local/bin/otelnet
	rm -f /etc/otelnet.conf.example
	@echo "Uninstall complete"

# Debug build (clean first to ensure all files are compiled with DEBUG flag)
.PHONY: debug
debug:
	@echo "Building in DEBUG mode..."
	@$(MAKE) clean
	@$(MAKE) DEBUG=1

# Static build
.PHONY: static
static: $(TARGET_STATIC)

# Link otelnet_static
$(TARGET_STATIC): $(OBJ_DIR) $(OBJECTS_STATIC)
	@echo "Linking $(TARGET_STATIC) (static)..."
	$(CC) $(LDFLAGS) -static $(OBJECTS_STATIC) $(LIBS) -o $(TARGET_STATIC)
	@echo "Build complete: $(TARGET_STATIC)"

# Show variables (for debugging Makefile)
.PHONY: show
show:
	@echo "CC:       $(CC)"
	@echo "CFLAGS:   $(CFLAGS)"
	@echo "SOURCES:  $(SOURCES)"
	@echo "OBJECTS:  $(OBJECTS)"
	@echo "TARGET:   $(TARGET)"

# Help
.PHONY: help
help:
	@echo "otelnet Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build otelnet (default)"
	@echo "  clean     - Remove build artifacts"
	@echo "  debug     - Build with debug symbols"
	@echo "  static    - Build statically linked otelnet_static"
	@echo "  install   - Install to system (requires root)"
	@echo "  uninstall - Remove from system (requires root)"
	@echo "  show      - Show Makefile variables"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Usage:"
	@echo "  make debug    - Build with debug symbols and logging"
