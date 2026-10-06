# libatree — GNU Makefile for POSIX toolchains (gcc, clang). Windows/MSVC uses
# CMakeLists.txt. Compatible with GNU make 3.81.
#
# Targets: all check check-asan check-ubsan check-tsan check-valgrind
#          check-header bench fuzz format format-check install clean
# Variables: CC CXX AR MODE=debug|release WERROR=1|0 BUILD=dir PREFIX=/usr/local
#            SAN="<sanitizer flags>" (internal; used by the check-* targets)

CC      ?= cc
CXX     ?= c++
AR      ?= ar
MODE    ?= debug
WERROR  ?= 1
BUILD   ?= build
PREFIX  ?= /usr/local
DESTDIR ?=
CLANG_FORMAT ?= clang-format

VERSION       := 0.1.0
SONAME_MAJOR  := 0

UNAME_S := $(shell uname -s)

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
            -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wcast-align \
            -Wpointer-arith -Wwrite-strings -Wformat=2 -Wundef -Wvla \
            -Wswitch-enum -Wswitch-default -Wfloat-equal -Wdouble-promotion
ifeq ($(WERROR),1)
WARNINGS += -Werror
endif

ifeq ($(MODE),release)
OPT := -O2 -g -DNDEBUG
else
OPT := -O0 -g
endif

CPPFLAGS += -Iinclude -Isrc -DATREE_BUILDING
CFLAGS   += -std=c99 $(OPT) $(WARNINGS) -fPIC -fvisibility=hidden -MMD -MP $(SAN)
LDFLAGS  += $(SAN)

ifeq ($(UNAME_S),Darwin)
SHLIB      := $(BUILD)/libatree.$(SONAME_MAJOR).dylib
SHLIB_LINK := $(BUILD)/libatree.dylib
SHFLAGS    := -dynamiclib -install_name @rpath/libatree.$(SONAME_MAJOR).dylib \
              -compatibility_version $(SONAME_MAJOR).0 -current_version $(VERSION)
else
SHLIB      := $(BUILD)/libatree.so.$(VERSION)
SHLIB_LINK := $(BUILD)/libatree.so
SHFLAGS    := -shared -Wl,-soname,libatree.so.$(SONAME_MAJOR)
endif
STLIB := $(BUILD)/libatree.a

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRCS))
TEST_DEPS := $(TEST_BINS:=.d)

BENCH_SRCS := $(wildcard bench/*.c)
BENCH_BINS := $(patsubst bench/%.c,$(BUILD)/bench/%,$(BENCH_SRCS))

FUZZ_SRCS := $(wildcard fuzz/*.c)
FUZZ_BINS := $(patsubst fuzz/%.c,$(BUILD)/fuzz/%,$(FUZZ_SRCS))

FORMAT_FILES := $(wildcard include/*.h src/*.c src/*.h tests/*.c tests/*.h tests/*.cpp \
                           bench/*.c bench/*.h fuzz/*.c extras/*.h)

.PHONY: all static shared check check-asan check-ubsan check-tsan check-valgrind \
        check-header bench fuzz format format-check install clean help

all: static shared

static: $(STLIB)
shared: $(SHLIB) $(SHLIB_LINK)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(STLIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(OBJS)

$(SHLIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(SHFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(SHLIB_LINK): $(SHLIB)
	ln -sf $(notdir $(SHLIB)) $@

# ---- tests ------------------------------------------------------------------

$(BUILD)/tests/%: tests/%.c $(STLIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MF $@.d -o $@ $< $(STLIB) $(LDFLAGS)

check: $(TEST_BINS) check-header
	@status=0; for t in $(TEST_BINS); do \
	    echo "RUN $$t"; $$t || status=1; \
	done; \
	if [ $$status -ne 0 ]; then echo "SOME TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED ($(words $(TEST_BINS)) suites)"

# The public header must compile and link as strictly conforming C99/C11/C17 and as C++.
check-header: $(STLIB)
	@mkdir -p $(BUILD)/tests
	@for std in c99 c11 c17; do \
	    echo "header check -std=$$std"; \
	    $(CC) -std=$$std -Wall -Wextra -Wpedantic -Werror -Iinclude $(SAN) \
	        -o $(BUILD)/tests/header_$$std tests/header_c.c $(STLIB) $(LDFLAGS) || exit 1; \
	    $(BUILD)/tests/header_$$std || exit 1; \
	done
	@echo "header check C++"
	@$(CXX) -std=c++11 -Wall -Wextra -Wpedantic -Werror -Iinclude $(SAN) \
	    -o $(BUILD)/tests/header_cxx tests/header_cxx.cpp $(STLIB) $(LDFLAGS)
	@$(BUILD)/tests/header_cxx

check-asan:
	$(MAKE) BUILD=$(BUILD)-asan \
	    SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" check

check-ubsan:
	$(MAKE) BUILD=$(BUILD)-ubsan \
	    SAN="-fsanitize=undefined -fno-sanitize-recover=all" check

# Thread tests only (TSan is slow); a no-op until tests/test_threads.c exists.
check-tsan:
	@if [ -f tests/test_threads.c ]; then \
	    $(MAKE) BUILD=$(BUILD)-tsan SAN="-fsanitize=thread" $(BUILD)-tsan/tests/test_threads && \
	    $(BUILD)-tsan/tests/test_threads; \
	else echo "check-tsan: no tests/test_threads.c yet"; fi

check-valgrind: $(TEST_BINS)
	@if command -v valgrind >/dev/null 2>&1; then \
	    for t in $(TEST_BINS); do \
	        echo "VALGRIND $$t"; \
	        valgrind --quiet --error-exitcode=1 --leak-check=full --show-leak-kinds=all \
	            --errors-for-leak-kinds=all $$t || exit 1; \
	    done; echo "VALGRIND CLEAN"; \
	else echo "check-valgrind: valgrind not installed on this machine (CI runs it)"; fi

# ---- benchmarks and fuzzing --------------------------------------------------

$(BUILD)/bench/%: bench/%.c $(STLIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -std=c99 -O2 -g $(WARNINGS) -o $@ $< $(STLIB) $(LDFLAGS)

bench: $(BENCH_BINS)
	@if [ -z "$(BENCH_SRCS)" ]; then echo "bench: nothing in bench/ yet"; fi

$(BUILD)/fuzz/%: fuzz/%.c $(STLIB)
	@mkdir -p $(dir $@)
	clang $(CPPFLAGS) -std=c99 -O1 -g -fsanitize=fuzzer,address,undefined -o $@ $< $(STLIB)

fuzz: $(FUZZ_BINS)
	@if [ -z "$(FUZZ_SRCS)" ]; then echo "fuzz: nothing in fuzz/ yet"; fi

# ---- formatting --------------------------------------------------------------

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_FILES)

# ---- install -----------------------------------------------------------------

install: all
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 644 include/atree.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 $(STLIB) $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(SHLIB) $(DESTDIR)$(PREFIX)/lib/
	ln -sf $(notdir $(SHLIB)) $(DESTDIR)$(PREFIX)/lib/$(notdir $(SHLIB_LINK))
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' atree.pc.in \
	    > $(DESTDIR)$(PREFIX)/lib/pkgconfig/atree.pc

clean:
	rm -rf $(BUILD) $(BUILD)-asan $(BUILD)-ubsan $(BUILD)-tsan

help:
	@echo "targets: all check check-asan check-ubsan check-tsan check-valgrind check-header"
	@echo "         bench fuzz format format-check install clean"
	@echo "vars:    CC CXX MODE=debug|release WERROR=1|0 BUILD=dir PREFIX=path"

-include $(DEPS) $(TEST_DEPS)
