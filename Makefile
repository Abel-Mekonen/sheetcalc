# sheetcalc — static lib + CLI
# hand-maintained; don't autogen this

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -g
AR      ?= ar

PREFIX  ?= /usr/local

SRCDIR   = src
INCDIR   = include
OBJDIR   = build/obj
LIBDIR   = build/lib
BINDIR   = build/bin

SRCS     = $(wildcard $(SRCDIR)/*.c)
OBJS     = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
LIB      = $(LIBDIR)/libsheetcalc.a
CLI      = $(BINDIR)/sheetcalc

CFLAGS  += -I$(INCDIR)

.PHONY: all clean install uninstall test check fuzz lib cli

all: lib cli

lib: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(LIBDIR)
	$(AR) rcs $@ $^

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

cli: $(LIB)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) tools/cli.c -L$(LIBDIR) -lsheetcalc -lm -o $(CLI)

gen_pocs: $(LIB)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) tools/gen_pocs.c -L$(LIBDIR) -lsheetcalc -lm -o $(BINDIR)/gen_pocs

clean:
	rm -rf build *.o *.a

test: $(LIB)
	$(CC) $(CFLAGS) tests/test_basic.c -L$(LIBDIR) -lsheetcalc -lm -o build/test_basic
	$(CC) $(CFLAGS) tests/test_formula.c -L$(LIBDIR) -lsheetcalc -lm -o build/test_formula
	$(CC) $(CFLAGS) tests/test_eval.c -L$(LIBDIR) -lsheetcalc -lm -o build/test_eval
	./build/test_basic
	./build/test_formula
	./build/test_eval

check: test

install: $(LIB) cli
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(INCDIR)/sheetcalc.h $(DESTDIR)$(PREFIX)/include/
	install -m 755 $(CLI) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libsheetcalc.a
	rm -f $(DESTDIR)$(PREFIX)/include/sheetcalc.h
	rm -f $(DESTDIR)$(PREFIX)/bin/sheetcalc

# asan build for development
asan:
	$(MAKE) CFLAGS="-Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$(INCDIR)"

# ---------------------------------------------------------------------------
# notes (for future me)
# ---------------------------------------------------------------------------
# src layout:
#   core parser eval builtins value buf formula validate convert format
#   util io checksum refgraph depsolve
#
# tests live under tests/ — add new ones to the test target above
# fuzz harnesses are built separately via .clusterfuzzlite/build.sh
#
# macos: if you hit weird ar errors, `brew install binutils` and AR=gar
# linux: cc + ar should just work
#
# example:
#   make
#   make test
#   make install PREFIX=$$HOME/.local
#   make asan && ./build/bin/sheetcalc ...

# debug build (uncomment to use):
# debug: CFLAGS += -O0 -DDEBUG -g3
# debug: clean all

# shared lib — not yet, would need -fPIC on objs and a .so/.dylib link rule
# TODO: csb_replay target once tools/replay.c lands

# keep OBJDIR around between incremental builds; `make clean` wipes build/
