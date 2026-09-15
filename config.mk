# smd version
VERSION = 1.0

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

GTKINC = `pkg-config --cflags gtk+-3.0`
GTKLIB = `pkg-config --libs gtk+-3.0`

# includes and libs
INCS = $(GTKINC)
LIBS = $(GTKLIB) -lm

# flags
CPPFLAGS = -DVERSION=\"$(VERSION)\" -D_DEFAULT_SOURCE -D_GNU_SOURCE
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -O2 $(INCS) $(CPPFLAGS)
LDFLAGS += $(LIBS)

# compiler
CC := clang
