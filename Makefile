PREFIX ?= /usr/local
LIBEXECDIR ?= $(PREFIX)/libexec
BINDIR ?= $(PREFIX)/bin
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror

all: docker-bindctl-helper

docker-bindctl-helper: bindns-helper.c
	$(CC) $(CFLAGS) -o $@ $<

install: docker-bindctl-helper
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBEXECDIR)
	install -m 0755 docker-bindctl $(DESTDIR)$(BINDIR)/docker-bindctl
	install -m 0755 docker-bindctl-helper $(DESTDIR)$(LIBEXECDIR)/docker-bindctl-helper

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/docker-bindctl $(DESTDIR)$(LIBEXECDIR)/docker-bindctl-helper

clean:
	rm -f docker-bindctl-helper

.PHONY: all install uninstall clean
