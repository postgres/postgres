# Postgres documentation makefile
# $Header: /cvsroot/pgsql/doc/src/Makefile,v 1.18 2001/03/27 16:34:14 momjian Exp $

subdir = doc/src
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

TAR= tar
ZIP= gzip
ZIPSUFFIX= gz
TAREXCLUDE= --exclude=Makefile --exclude='*.sgml' --exclude=ref

SRC= admin developer reference programmer tutorial user  postgres

TARGETS= $(SRC:%=%.tar.$(ZIPSUFFIX))

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

admin.tar developer.tar reference.tar user.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml $(basename $@).html
	cd sgml && $(TAR) -cf ../$@ $(TAREXCLUDE) *.html

programmer.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml programmer.html
	abssrcdir=`cd $(srcdir) && pwd`; \
	cd sgml && $(TAR) -cf ../$@ $(TAREXCLUDE) *.html -C $$abssrcdir/graphics catalogs.gif connections.gif

tutorial.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml tutorial.html
	abssrcdir=`cd $(srcdir) && pwd`; \
	cd sgml && $(TAR) -cf ../$@ $(TAREXCLUDE) *.html -C $$abssrcdir/graphics clientserver.gif

postgres.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml postgres.html
	abssrcdir=`cd $(srcdir) && pwd`; \
	cd sgml && $(TAR) -cf ../$@ $(TAREXCLUDE) *.html -C $$abssrcdir/graphics catalogs.gif clientserver.gif connections.gif

man.tar:
	$(MAKE) -C sgml man
	$(TAR) -cf $@ -C sgml man1 manl

# Generic production rules

# Compressed file

%.$(ZIPSUFFIX):	%
	$(ZIP) -f $<
