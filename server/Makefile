# atreed — continuous-query server over libatree, on pogocache's networking
# core (deps/pogocache, MIT). POSIX only. The library is built from the
# parent directory in release mode; nothing here touches it.
#
#   make            build atreed
#   make check      build and run the protocol tests (needs python3)
#   make clean
CC      ?= cc
BUILD   ?= build
LIBDIR  := ../build-release
LIB     := $(LIBDIR)/libatree.a

DEPDIR  := deps/pogocache
DEPSRCS := $(DEPDIR)/net.c $(DEPDIR)/buf.c $(DEPDIR)/conn.c $(DEPDIR)/parse.c \
           $(DEPDIR)/resp.c $(DEPDIR)/postgres.c $(DEPDIR)/http.c $(DEPDIR)/hashmap.c \
           $(DEPDIR)/util.c $(DEPDIR)/args.c $(DEPDIR)/sys.c $(DEPDIR)/xmalloc.c \
           $(DEPDIR)/stats.c $(DEPDIR)/tls.c $(DEPDIR)/uring.c
SRCS    := atreed.c json.c
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(SRCS)) $(patsubst $(DEPDIR)/%.c,$(BUILD)/deps/%.o,$(DEPSRCS))

DEPFLAGS := -DNOOPENSSL -DNOURING -DNOMIMALLOC -DNOJEMALLOC
CPPFLAGS += -I../include -I. -I$(DEPDIR) $(DEPFLAGS)
# Our files hold to the warning set of the library; the vendored files are
# compiled as they ship (gnu11, -Wall -Wextra), with the same -Werror.
OURFLAGS := -std=c99 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
            -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wpointer-arith \
            -Wwrite-strings -Wformat=2 -Wundef -Wvla -Werror
DEPCFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Werror
LDLIBS  += -lm -lpthread

.PHONY: all check clean lib

all: $(BUILD)/atreed

lib:
	$(MAKE) -C .. MODE=release static

$(LIB): lib

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(OURFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/deps/%.o: $(DEPDIR)/%.c | $(BUILD)/deps
	$(CC) $(CPPFLAGS) $(DEPCFLAGS) -MMD -MP -c -o $@ $<

$(BUILD) $(BUILD)/deps:
	mkdir -p $@

$(BUILD)/atreed: $(OBJS) $(LIB)
	$(CC) -o $@ $(OBJS) $(LIB) $(LDLIBS)

check: $(BUILD)/atreed
	python3 tests/run.py $(BUILD)/atreed

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
