PREFIX ?= /usr/local
CC ?= cc
PKGS = gtk+-3.0 gtk-layer-shell-0 json-glib-1.0 libcurl
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=gnu11 $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS)) -lm

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o) build/resources.o
BIN = build/muslimboard

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -MMD -c -o $@ $<

build/resources.c: data/muslimboard.gresource.xml $(wildcard data/*.json data/*.css) | build
	glib-compile-resources --sourcedir=data --target=$@ --generate-source $<

build/resources.o: build/resources.c
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

build:
	mkdir -p build

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/muslimboard

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/muslimboard

# clangd reads this; run `make compile_commands.json` after changing PKGS or adding files
compile_commands.json: Makefile build/resources.c
	@printf '[' > $@
	@sep=; for f in $(SRC); do \
	  printf '%s\n {"directory": "%s", "file": "%s", "arguments": ["$(CC)", "-c", "%s", "-Isrc", "-Ibuild"%s]}' \
	    "$$sep" "$(CURDIR)" "$$f" "$$f" "$(foreach a,$(CFLAGS),$(if $(filter -I% -D% -std% -pthread,$(a)),$(comma) \"$(a)\"))" >> $@; sep=,; done
	@printf '\n]\n' >> $@

comma := ,

clean:
	rm -rf build compile_commands.json

-include $(OBJ:.o=.d)
.PHONY: all install uninstall clean
