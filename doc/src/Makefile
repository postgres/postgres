# Postgres documentation makefile
# $Header: /cvsroot/pgsql/doc/src/Makefile,v 1.22 2001/09/18 12:08:26 petere Exp $

subdir = doc/src
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

ZIPSUFFIX= gz

BOOKS = admin developer reference programmer tutorial user  postgres

TARGETS= $(BOOKS:%=%.tar.$(ZIPSUFFIX))

.PRECIOUS:
.PHONY: install all sources clean distclean

install:
	$(MAKE) all
	(mv -f *.$(ZIPSUFFIX) ..)

clean:
	$(MAKE) -C sgml clean

distclean:
	$(MAKE) -C sgml distclean

all: $(TARGETS)

sources:
	$(MAKE) sources.tar.$(ZIPSUFFIX)

sources.tar:
	$(TAR) -cf $@ sgml graphics

$(addsuffix .tar, $(BOOKS)):
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml $(basename $@).html
	$(MAKE) -C sgml $(basename $@).html JADEFLAGS='-V html-manifest'
	cd sgml && $(TAR) -c -f ../$@ -T HTML.manifest *.gif *.css

man.tar:
	$(MAKE) -C sgml man
	$(TAR) -c -f $@ -C sgml man1 man$(sqlmansect_dummy)
