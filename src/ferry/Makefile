# Ferry — first-boot migration engine.
#
# The shell tools need no build. This compiles the two C helpers that
# shell cannot be (a binary registry-hive B-tree, and NTFS attribute
# classification), into build/ next to the scripts, which is exactly
# where ferry-common.sh looks for them when running from the source
# tree. `make check` shellcheck-syntaxes every script and runs the
# offline test suite against a synthetic Windows tree.

CC      ?= cc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra
BUILD   := build
HELPERS := $(BUILD)/ferry-hivepeek $(BUILD)/ferry-winattr

SCRIPTS := ferry ferry-detect ferry-wifi ferry-files ferry-firefox \
           ferry-settings ferry-report ferry-hive ferry-dpapi ferry-common.sh

PREFIX  ?= /usr
LIBDIR  := $(DESTDIR)$(PREFIX)/lib/ferry
SHAREDIR:= $(DESTDIR)$(PREFIX)/share/ferry
BINDIR  := $(DESTDIR)$(PREFIX)/bin

.PHONY: all check test syntax clean install
all: $(HELPERS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ferry-hivepeek: hivepeek.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/ferry-winattr: winattr.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

# Every script must parse under a strict POSIX shell, not just bash.
syntax:
	@for s in $(SCRIPTS); do \
	    sh -n $$s && dash -n $$s && echo "  ok  $$s" || exit 1; \
	done

check: all syntax test

test: all
	@sh tests/run.sh

install: all
	install -d $(LIBDIR) $(SHAREDIR) $(BINDIR)
	install -m755 $(HELPERS) $(LIBDIR)/
	@for s in $(SCRIPTS); do install -m755 $$s $(LIBDIR)/$$s; done
	install -m644 data/*.tab $(SHAREDIR)/
	ln -sf $(PREFIX)/lib/ferry/ferry $(BINDIR)/ferry

clean:
	rm -rf $(BUILD)
