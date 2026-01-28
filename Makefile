PREFIX = $(MIX_APP_PATH)/priv
BUILD  = $(MIX_APP_PATH)/obj

# Check that we're on a supported build platform
ifeq ($(CROSSCOMPILE),)
    # Not crosscompiling, so check compilation platform.
    UNAME_S := $(shell uname -s 2>/dev/null)

    ifeq ($(findstring Windows,$(UNAME_S)),Windows)
        EXE = .exe
    endif
endif

# Compiler and flags
LDFLAGS +=
CFLAGS ?= -O2 -Wall -Wextra
CC ?= $(CROSSCOMPILE)-gcc

EXECUTABLE = $(PREFIX)/odu$(EXE)
SRCDIR = c_src
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/logger.c $(SRCDIR)/protocol.c $(SRCDIR)/process.c
HEADERS = $(SRCDIR)/odu.h
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(BUILD)/%.o)

calling_from_make:
	mix compile

all: install

install: $(PREFIX) $(BUILD) $(EXECUTABLE)

$(BUILD)/%.o: c_src/%.c
	@echo " CC $(notdir $@)"
	$(CC) -c $(CFLAGS) -o $@ $<

$(EXECUTABLE): $(OBJECTS)
	@echo " LD $(notdir $@)"
	$(CC) $^ $(LDFLAGS) -o $@

$(PREFIX) $(BUILD):
	mkdir -p $@

mix_clean:
	$(RM) $(EXECUTABLE) $(OBJECTS)

clean:
	mix clean

.PHONY: all clean mix_clean calling_from_make install

# Don't echo commands unless the caller exports "V=1"
${V}.SILENT:
