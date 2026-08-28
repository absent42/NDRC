# SPDX-License-Identifier: GPL-3.0-or-later
# NDRC - Next DAAD Reborn Compiler
# Copyright (C) 2026 Dan Gibson.

ifeq ($(origin CC),default)
CC := gcc
endif
STD     ?= c17
OPT     ?= -O2
CFLAGS  += -std=$(STD) -Wall -Wextra -Wpedantic $(OPT) -Isrc
EXTRA   ?=

ifeq ($(OS),Windows_NT)
  SHELL := cmd.exe
  .SHELLFLAGS := /c
  EXE  := .exe
  FIX   = $(subst /,\,$1)
  RMF  := del /q /f
else
  EXE  :=
  FIX   = $1
  RMF  := rm -f
endif

LIB_SRC  := $(filter-out src/main.c,$(wildcard src/*.c)) $(wildcard src/back/*.c) \
            $(wildcard src/front/*.c)
LIB_OBJ  := $(LIB_SRC:.c=.o)
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(TEST_SRC:.c=$(EXE))

DEPFLAGS = -MMD -MP

ifeq ($(strip $(TEST_SRC)),)
$(error no test sources found - tests/test_*.c matched nothing)
endif

.PHONY: all test sanitize clean

all: $(LIB_OBJ) ndrc$(EXE)

LDLIBS := -lm

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(EXTRA) -c -o $@ $<

tests/%$(EXE): tests/%.c $(LIB_OBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF tests/$*.d $(EXTRA) -Itests -o $@ $< $(LIB_OBJ) $(LDLIBS)

-include $(wildcard src/*.d) $(wildcard src/back/*.d) $(wildcard src/front/*.d) \
         $(wildcard tests/*.d)

ndrc$(EXE): src/main.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $(EXTRA) -o $@ src/main.o $(LIB_OBJ) $(LDLIBS)

# ndrc$(EXE) too: main.c is filtered out of LIB_SRC, so without this the
# standard gate never compiles it (a broken main.c once reached a commit).
test: ndrc$(EXE) $(TEST_BIN)
	$(foreach t,$(TEST_BIN),$(call FIX,$(t)) &&) echo All test suites passed

sanitize:
	$(MAKE) clean
	$(MAKE) test OPT="-O1 -g -fno-omit-frame-pointer" \
	             EXTRA="-fsanitize=address,undefined"

clean:
	-$(RMF) $(call FIX,$(LIB_OBJ))
	-$(RMF) $(call FIX,$(LIB_OBJ:.o=.d))
	-$(RMF) $(call FIX,$(TEST_BIN))
	-$(RMF) $(call FIX,$(TEST_SRC:.c=.d))
	-$(RMF) $(call FIX,src/main.o)
	-$(RMF) $(call FIX,src/main.d)
	-$(RMF) $(call FIX,ndrc$(EXE))
