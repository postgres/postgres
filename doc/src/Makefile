# Postgres documentation makefile
# $Header: /cvsroot/pgsql/doc/src/Makefile,v 1.29 2002/07/28 15:19:59 petere Exp $

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

clean distclean maintainer-clean:
	$(MAKE) -C sgml $@

all: $(TARGETS)

sources:
	$(MAKE) sources.tar.$(ZIPSUFFIX)

sources.tar:
	$(TAR) -cf $@ sgml graphics

# Build twice to resolve index entries
$(addsuffix .tar, $(BOOKS)):
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml $(basename $@).html
	$(MAKE) -C sgml $(basename $@).html JADEFLAGS='-V html-manifest'
	cd sgml && $(TAR) -cf ../$@ `cat HTML.manifest` `echo *.gif | grep -v '\*'` *.css

man.tar:
	$(MAKE) -C sgml man
	$(TAR) -cf $@ -C sgml man1 man$(sqlmansect_dummy)
