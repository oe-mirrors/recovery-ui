#!/usr/bin/make -f
#
# Copyright (C) 2016 Dream Property GmbH, Germany
#                    http://www.dream-multimedia-tv.de/
#

prefix ?= /usr/local
exec_prefix ?= $(prefix)
bindir ?= $(exec_prefix)/bin
sysconfdir ?= $(prefix)/etc
localstatedir ?= $(prefix)/var
runstatedir ?= $(localstatedir)/run

override CFLAGS := $(CFLAGS) -Wall -Wextra -std=c99
override CPPFLAGS := $(CPPFLAGS) -DNDEBUG -MD

INITSCRIPT := recovery-ui.init
TARGETS := recovery-ui

default: $(INITSCRIPT) $(TARGETS)

recovery-ui: recovery-ui.o lcd.o

recovery-ui.init: recovery-ui.init.in Makefile
	sed -e 's,@bindir@,$(bindir),g' \
	    -e 's,@sysconfdir@,$(sysconfdir),g' \
	    -e 's,@runstatedir@,$(runstatedir),g' \
	    < $< > $@

clean:
	$(RM) $(INITSCRIPT) $(TARGETS) *.[do]

install: $(TARGETS)
	install -d $(DESTDIR)$(sysconfdir)/init.d
	install -m 755 $(INITSCRIPT) $(DESTDIR)$(sysconfdir)/init.d/recovery-ui
	install -d $(DESTDIR)$(bindir)
	install -m 755 $(TARGETS) $(DESTDIR)$(bindir)

-include $(wildcard *.d)
