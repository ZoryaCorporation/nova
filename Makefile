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
CFLAGS  += -Wconversion -Wshadow -Wformat=2
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
INCLUDES = -I$(NOVA_INC)

# Zorya SDK sources we depend on
ZORYA_SOURCES = \
    $(ZORYA_SRC)/nxh.c \
    $(ZORYA_SRC)/dagger.c \
    $(ZORYA_SRC)/weave.c

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
    $(NOVA_SRC)/nova_trace.c

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
    $(NOVA_SRC)/nova_lib_debug.c

# Main interpreter
NOVA_MAIN = $(NOVA_SRC)/nova.c

# Object files
ZORYA_OBJECTS = $(patsubst $(ZORYA_SRC)/%.c,$(NOVA_BUILD)/zorya_%.o,$(ZORYA_SOURCES))
NOVA_OBJECTS  = $(patsubst $(NOVA_SRC)/%.c,$(NOVA_BUILD)/%.o,$(NOVA_SOURCES))
LIB_OBJECTS   = $(patsubst $(NOVA_SRC)/%.c,$(NOVA_BUILD)/%.o,$(NOVA_LIB_SOURCES))
MAIN_OBJECT   = $(NOVA_BUILD)/nova.o

ALL_OBJECTS = $(ZORYA_OBJECTS) $(NOVA_OBJECTS) $(LIB_OBJECTS)

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

# ============================================================================
# RULES
# ============================================================================

.PHONY: all lib clean test trace install uninstall dirs

all: dirs $(TARGET)
	@echo "========================================="
	@echo "  Nova $(shell cat VERSION 2>/dev/null || echo '0.1.0') built successfully"
	@echo "  Binary: $(TARGET)"
	@echo "========================================="

lib: dirs $(LIBRARY)
	@echo "  Static library: $(LIBRARY)"

dirs:
	@mkdir -p $(NOVA_BUILD) $(NOVA_BIN)

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

# Clean
clean:
	rm -rf $(NOVA_BUILD) $(NOVA_BIN)
	@echo "  Cleaned."

# Tests
test: all
	@echo "Running Nova test suite..."
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
install: all lib
	install -d $(BINDIR) $(LIBDIR) $(INCDIR)
	install -m 755 $(TARGET) $(BINDIR)/
	install -m 644 $(LIBRARY) $(LIBDIR)/
	install -m 644 $(NOVA_INC)/nova/*.h $(INCDIR)/

uninstall:
	rm -f $(BINDIR)/nova
	rm -f $(LIBDIR)/libnova.a
	rm -rf $(INCDIR)

# ============================================================================
# DEPENDENCIES (auto-generated on full build)
# ============================================================================

-include $(ALL_OBJECTS:.o=.d)
-include $(MAIN_OBJECT:.o=.d)
