# Builds the reference be-tree (reference/be-tree, read-only) out of tree and
# links the comparison driver against it. Optional tooling, like
# bench/rust_compare: needs the reference checkout, not part of the library.
#
#   make -C bench/betree_compare
#   bench/betree_compare/build/betree_compare DEFS EXPRS EVENTS [--repeat R]
#
# The be-tree ships with its flex/bison output checked in, so neither tool is
# needed (the built-in rules are off so make never tries to regenerate it).
# It is compiled with its own flags (-O3 -std=gnu11) and its warnings
# silenced; the driver uses this project's warning set.

.SUFFIXES:

REF    ?= ../../reference/be-tree
BUILD  ?= build
CC     ?= cc

BETREE_SRCS := $(wildcard $(REF)/src/*.c)
BETREE_OBJS := $(patsubst $(REF)/src/%.c,$(BUILD)/obj/%.o,$(BETREE_SRCS))

WARNINGS := -Wall -Wextra -Wshadow -Wcast-qual -Wpointer-arith -Wstrict-prototypes \
            -Wmissing-prototypes -Wundef -Wconversion -Wsign-conversion -Werror

all: $(BUILD)/betree_compare

$(BUILD)/obj/%.o: $(REF)/src/%.c | $(BUILD)/obj
	$(CC) -O3 -g -std=gnu11 -w -I$(REF)/src -c -o $@ $<

$(BUILD)/libbetree.a: $(BETREE_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/betree_compare: betree_compare.c $(BUILD)/libbetree.a
	$(CC) -std=c11 -O2 -g $(WARNINGS) -isystem $(REF)/src -o $@ betree_compare.c $(BUILD)/libbetree.a -lm

$(BUILD)/obj:
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
