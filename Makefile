PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pedantic
LDFLAGS ?=

SRC = src/main.c
BIN = sntc

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all install uninstall clean
