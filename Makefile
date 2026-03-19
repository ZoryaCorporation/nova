# ============================================================================
# Nova Language - Build System
#
# Author: Anthony Taliento
# Date:   2026-02-05
# Copyright (c) 2026 Zorya Corporation
# License: MIT
#
# Usage:
#   make              Build nova interpreter
#   make lib          Build libnova.a static library
#   make clean        Clean build artifacts
#   make test         Run test suite
#   make install      Install to PREFIX (default: /usr/local)
# ============================================================================

# Compiler and flags (ZORYA-C v2.0.0 strict)
CC       = gcc
CFLAGS   = -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic
CFLAGS  += -Wconversion -Wshadow -Wformat=2 -Wno-format-nonliteral
CFLAGS  += -Wunused-parameter -Wswitch-default
CFLAGS  += -Wno-unknown-pragmas
CFLAGS  += -fstack-protector-strong
CFLAGS  += -fPIC

# Debug / Release
ifdef DEBUG
    CFLAGS  += -g -O0 -DNOVA_DEBUG=1 -fsanitize=address,undefined
    LDFLAGS += -fsanitize=address,undefined
else
    CFLAGS  += -O2 -DNDEBUG -flto
    LDFLAGS += -flto
endif

# Trace instrumentation (compile with TRACE=1 or use 'make trace')
ifdef TRACE
    CFLAGS += -DNOVA_TRACE=1
endif

# Directories
NOVA_ROOT    = .
NOVA_SRC     = $(NOVA_ROOT)/src
NOVA_INC     = $(NOVA_ROOT)/include
NOVA_BUILD   = $(NOVA_ROOT)/build
NOVA_BIN     = $(NOVA_ROOT)/bin
NOVA_TEST    = $(NOVA_ROOT)/tests

# Zorya SDK (vendored into this repo)
ZORYA_SRC    = $(NOVA_ROOT)/src/zorya

# Include paths (all local now)
INCLUDES = -I$(NOVA_INC) -I$(NOVA_SRC)

# PAL (Platform Abstraction Layer) source selection
# Auto-detect from OS, or override with PAL=posix|win32|stub
ifndef PAL
    UNAME_S := $(shell uname -s 2>/dev/null)
    ifeq ($(UNAME_S),Linux)
        PAL = posix
    else ifeq ($(UNAME_S),Darwin)
        PAL = posix
    else ifeq ($(findstring BSD,$(UNAME_S)),BSD)
        PAL = posix
    else ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
        PAL = win32
    else ifeq ($(findstring MSYS,$(UNAME_S)),MSYS)
        PAL = win32
    else
        PAL = stub
    endif
endif

PAL_SOURCE = $(ZORYA_SRC)/pal_$(PAL).c

# Zorya SDK sources we depend on
ZORYA_SOURCES = \
    $(ZORYA_SRC)/nxh.c \
    $(ZORYA_SRC)/dagger.c \
    $(ZORYA_SRC)/weave.c \
    $(ZORYA_SRC)/sqlite3.c \
    $(PAL_SOURCE)

# Nova core sources
NOVA_SOURCES = \
    $(NOVA_SRC)/nova_error.c \
    $(NOVA_SRC)/nova_lex.c \
    $(NOVA_SRC)/nova_pp.c \
    $(NOVA_SRC)/nova_ast_row.c \
    $(NOVA_SRC)/nova_parse.c \
    $(NOVA_SRC)/nova_parse_row.c \
    $(NOVA_SRC)/nova_compile.c \
    $(NOVA_SRC)/nova_opt.c \
    $(NOVA_SRC)/nova_codegen.c \
    $(NOVA_SRC)/nova_opcode.c \
    $(NOVA_SRC)/nova_proto.c \
    $(NOVA_SRC)/nova_vm.c \
    $(NOVA_SRC)/nova_meta.c \
    $(NOVA_SRC)/nova_gc.c \
    $(NOVA_SRC)/nova_coroutine.c \
    $(NOVA_SRC)/nova_async.c \
    $(NOVA_SRC)/nova_ndp.c \
    $(NOVA_SRC)/nova_nini.c \
    $(NOVA_SRC)/nova_trace.c \
    $(NOVA_SRC)/nova_suggest.c \
    $(NOVA_SRC)/nova_shell.c \
    $(NOVA_SRC)/nova_task.c

# Standard library sources
NOVA_LIB_SOURCES = \
    $(NOVA_SRC)/nova_lib_base.c \
    $(NOVA_SRC)/nova_lib_math.c \
    $(NOVA_SRC)/nova_lib_string.c \
    $(NOVA_SRC)/nova_lib_table.c \
    $(NOVA_SRC)/nova_lib_io.c \
    $(NOVA_SRC)/nova_lib_os.c \
    $(NOVA_SRC)/nova_lib_package.c \
    $(NOVA_SRC)/nova_lib_coroutine.c \
    $(NOVA_SRC)/nova_lib_async.c \
    $(NOVA_SRC)/nova_lib_data.c \
    $(NOVA_SRC)/nova_lib_debug.c \
    $(NOVA_SRC)/nova_lib_net.c \
    $(NOVA_SRC)/nova_lib_sql.c \
    $(NOVA_SRC)/nova_lib_fs.c \
    $(NOVA_SRC)/nova_lib_nlp.c \
    $(NOVA_SRC)/nova_lib_tools.c

# CLI tools
NOVA_TOOL_SOURCES = \
    $(NOVA_SRC)/nova_tools.c

# Shared tool utility library (modular tools infrastructure)
NTOOL_SRC     = $(NOVA_SRC)/tools/shared
NTOOL_BUILD   = $(NOVA_BUILD)/tools
NTOOL_SOURCES = $(NTOOL_SRC)/ntool_common.c
NTOOL_OBJECTS = $(NTOOL_BUILD)/ntool_common.o
NTOOL_LIB     = $(NOVA_BUILD)/libnova_toolutil.a

# PAL object needed by tool utility library
PAL_OBJECT    = $(NOVA_BUILD)/zorya_pal_$(PAL).o

# Standalone tool binaries (modular tools)
TOOL_BIN_SRC     = $(NOVA_SRC)/tools
TOOL_BIN_SOURCES = $(wildcard $(TOOL_BIN_SRC)/n*.c)
TOOL_BIN_OBJECTS = $(patsubst $(TOOL_BIN_SRC)/%.c,$(NTOOL_BUILD)/%.o,$(TOOL_BIN_SOURCES))
TOOL_BIN_DIR     = $(NOVA_BIN)/tools
TOOL_BIN_TARGETS = $(patsubst $(TOOL_BIN_SRC)/%.c,$(TOOL_BIN_DIR)/%,$(TOOL_BIN_SOURCES))

# Main interpreter
NOVA_MAIN = $(NOVA_SRC)/nova.c

# Object files
ZORYA_OBJECTS = $(patsubst $(ZORYA_SRC)/%.c,$(NOVA_BUILD)/zorya_%.o,$(ZORYA_SOURCES))
NOVA_OBJECTS  = $(patsubst $(NOVA_SRC)/%.c,$(NOVA_BUILD)/%.o,$(NOVA_SOURCES))
LIB_OBJECTS   = $(patsubst $(NOVA_SRC)/%.c,$(NOVA_BUILD)/%.o,$(NOVA_LIB_SOURCES))
TOOL_OBJECTS  = $(patsubst $(NOVA_SRC)/%.c,$(NOVA_BUILD)/%.o,$(NOVA_TOOL_SOURCES))
MAIN_OBJECT   = $(NOVA_BUILD)/nova.o

ALL_OBJECTS = $(ZORYA_OBJECTS) $(NOVA_OBJECTS) $(LIB_OBJECTS) $(TOOL_OBJECTS) $(NTOOL_OBJECTS)

# Targets
TARGET     = $(NOVA_BIN)/nova
LIBRARY    = $(NOVA_BUILD)/libnova.a

# Install
PREFIX     ?= /usr/local
BINDIR      = $(PREFIX)/bin
LIBDIR      = $(PREFIX)/lib
INCDIR      = $(PREFIX)/include/nova

# Linker flags
LDLIBS = -lm

# Optional: libcurl for net module (disable with NOVA_NO_NET=1)
ifdef NOVA_NO_NET
    CFLAGS += -DNOVA_NO_NET
else
    LDLIBS += -lcurl
endif

# PAL link dependencies (dlopen needs -ldl on Linux/BSD)
ifeq ($(PAL),posix)
    LDLIBS += -ldl
endif

# Static linking on Windows (fold GCC/pthread runtime into binary)
# Disable LTO on Windows — MinGW LTO can produce broken CRT startup
# Set 8 MB stack (Windows default is 1 MB; Linux default is 8 MB)
ifeq ($(PAL),win32)
    LDFLAGS += -static -Wl,--stack,8388608
    CFLAGS  := $(filter-out -flto,$(CFLAGS))
    LDFLAGS := $(filter-out -flto,$(LDFLAGS))
endif

# ============================================================================
# RULES
# ============================================================================

.PHONY: all lib clean test trace install uninstall dirs tools

# ANSI colors for build output
CLR_RESET  = \033[0m
CLR_NOVA   = \033[1;38;5;40m
CLR_EMERALD= \033[38;5;35m
CLR_MUTED  = \033[38;5;245m
CLR_BOLD   = \033[1m
CLR_DIM    = \033[2m
CLR_SEP    = \033[38;5;35m

all: dirs $(TARGET)
	@printf "$(CLR_SEP)━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━$(CLR_RESET)\n"
	@printf "  $(CLR_NOVA)Nova $(shell cat VERSION 2>/dev/null || echo '0.2.0')$(CLR_RESET) built successfully\n"
	@printf "  $(CLR_MUTED)Binary:$(CLR_RESET) $(CLR_BOLD)$(TARGET)$(CLR_RESET)\n"
	@printf "$(CLR_SEP)━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━$(CLR_RESET)\n"

lib: dirs $(LIBRARY)
	@printf "  $(CLR_EMERALD)Static library:$(CLR_RESET) $(LIBRARY)\n"

dirs:
	@mkdir -p $(NOVA_BUILD) $(NOVA_BIN) $(NTOOL_BUILD) $(TOOL_BIN_DIR)

# Link interpreter
$(TARGET): $(ALL_OBJECTS) $(MAIN_OBJECT)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Static library (without main)
$(LIBRARY): $(ALL_OBJECTS)
	ar rcs $@ $^

# Compile Nova sources
$(NOVA_BUILD)/%.o: $(NOVA_SRC)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Compile Zorya SDK sources
$(NOVA_BUILD)/zorya_%.o: $(ZORYA_SRC)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Compile shared tool utility sources
$(NTOOL_BUILD)/%.o: $(NTOOL_SRC)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Build shared tool utility library (includes PAL for standalone use)
$(NTOOL_LIB): $(NTOOL_OBJECTS) $(PAL_OBJECT)
	ar rcs $@ $^

# Compile standalone tool sources
$(NTOOL_BUILD)/%.o: $(TOOL_BIN_SRC)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# Link standalone tool binaries
$(TOOL_BIN_DIR)/%: $(NTOOL_BUILD)/%.o $(NTOOL_LIB)
	$(CC) $(LDFLAGS) -o $@ $< -L$(NOVA_BUILD) -lnova_toolutil $(TOOL_BIN_LDLIBS)

# Tool-specific link flags (lighter than main LDLIBS)
TOOL_BIN_LDLIBS =
ifeq ($(PAL),posix)
    TOOL_BIN_LDLIBS += -ldl
endif

# Build all standalone tool binaries
tools: dirs $(NTOOL_LIB) $(TOOL_BIN_TARGETS)
	@printf "$(CLR_SEP)━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━$(CLR_RESET)\n"
	@printf "  $(CLR_NOVA)Nova Tools$(CLR_RESET) built: $(words $(TOOL_BIN_TARGETS)) binaries\n"
	@printf "  $(CLR_MUTED)Directory:$(CLR_RESET) $(CLR_BOLD)$(TOOL_BIN_DIR)/$(CLR_RESET)\n"
	@printf "$(CLR_SEP)━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━$(CLR_RESET)\n"

# Compile vendored SQLite3 with relaxed warnings (upstream code)
$(NOVA_BUILD)/zorya_sqlite3.o: $(ZORYA_SRC)/sqlite3.c
	$(CC) -std=c99 -O2 -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION $(INCLUDES) -c -o $@ $<

# Clean
clean:
	rm -rf $(NOVA_BUILD) $(NOVA_BIN) $(NTOOL_BUILD) $(TOOL_BIN_DIR)
	@printf "  $(CLR_MUTED)Cleaned.$(CLR_RESET)\n"

# Tests
test: all
	@printf "$(CLR_EMERALD)Running Nova test suite...$(CLR_RESET)\n"
	@if [ -d "$(NOVA_TEST)" ]; then \
		for f in $(NOVA_TEST)/test_*.n; do \
			echo "  Testing: $$f"; \
			$(TARGET) "$$f" || exit 1; \
		done; \
		echo "All tests passed."; \
	else \
		echo "  No tests found yet."; \
	fi

# Trace build (debug + trace instrumentation)
trace: clean
	@echo "Building Nova with trace instrumentation..."
	$(MAKE) TRACE=1
	@echo "========================================="
	@echo "  Nova TRACE build complete"
	@echo "  Usage: NOVA_TRACE=all ./bin/nova script.n"
	@echo "  Usage: NOVA_TRACE=vm,call,stack ./bin/nova script.n"
	@echo "  Usage: ./bin/nova --trace=vm,call script.n"
	@echo "========================================="

# Install
TOOLSDIR    = $(PREFIX)/lib/nova/tools

install: all lib tools
	@printf "\n$(CLR_SEP)━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━$(CLR_RESET)\n"
	@printf "  $(CLR_NOVA)Installing Nova $(shell cat VERSION 2>/dev/null || echo '0.2.0')$(CLR_RESET)\n"
	@printf "$(CLR_SEP)━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━$(CLR_RESET)\n"
	install -d $(BINDIR) $(LIBDIR) $(INCDIR) $(TOOLSDIR)
	install -m 755 $(TARGET) $(BINDIR)/
	install -m 644 $(LIBRARY) $(LIBDIR)/
	install -m 644 $(NOVA_INC)/nova/*.h $(INCDIR)/
	@for t in $(TOOL_BIN_DIR)/n*; do \
		[ -f "$$t" ] && install -m 755 "$$t" $(TOOLSDIR)/; \
	done
	@printf "\n  $(CLR_EMERALD)Binary$(CLR_RESET)   $(BINDIR)/nova\n"
	@printf "  $(CLR_EMERALD)Library$(CLR_RESET)  $(LIBDIR)/libnova.a\n"
	@printf "  $(CLR_EMERALD)Headers$(CLR_RESET)  $(INCDIR)/\n"
	@printf "  $(CLR_EMERALD)Tools$(CLR_RESET)    $(TOOLSDIR)/\n"
	@printf "\n$(CLR_SEP)━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━$(CLR_RESET)\n"
	@printf "  $(CLR_NOVA)Nova installed successfully!$(CLR_RESET)\n"
	@printf "  $(CLR_MUTED)Run$(CLR_RESET) $(CLR_BOLD)nova --version$(CLR_RESET) $(CLR_MUTED)to verify.$(CLR_RESET)\n"
	@printf "$(CLR_SEP)━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━$(CLR_RESET)\n\n"

uninstall:
	rm -f $(BINDIR)/nova
	rm -f $(LIBDIR)/libnova.a
	rm -rf $(INCDIR)
	rm -rf $(TOOLSDIR)
	@printf "  $(CLR_MUTED)Nova uninstalled from $(PREFIX)$(CLR_RESET)\n"

# ============================================================================
# DEPENDENCIES (auto-generated on full build)
# ============================================================================

-include $(ALL_OBJECTS:.o=.d)
-include $(MAIN_OBJECT:.o=.d)
