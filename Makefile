# smd - simple markdown viewer
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

SRC = smd.c
OBJ = $(SRC:.c=.o)

all: options smd

options:
	@echo smd build options:
	@echo "CC     = $(CC)"
	@echo "CFLAGS = $(CFLAGS)"

smd: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

$(OBJ): config.h config.mk arg.h

config.h:
	cp config.def.h $@

.c.o:
	$(CC) -c $(CFLAGS) $<

clean:
	rm -f smd $(OBJ)

distclean: clean
	rm -f config.h smd-$(VERSION).tar.gz

dist: distclean
	mkdir -p smd-$(VERSION)
	cp -R LICENSE Makefile config.mk config.def.h README TODO.md \
	    FAQ.md arg.h $(SRC) smd.1 smd-$(VERSION)
	tar -cf smd-$(VERSION).tar smd-$(VERSION)
	gzip smd-$(VERSION).tar
	rm -rf smd-$(VERSION)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f smd $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/smd
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < smd.1 > $(DESTDIR)$(MANPREFIX)/man1/smd.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/smd.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/smd
	rm -f $(DESTDIR)$(MANPREFIX)/man1/smd.1

.PHONY: all options clean distclean dist install uninstall
