# Postgres documentation makefile
# $Header: /cvsroot/pgsql/doc/src/Makefile,v 1.15 2000/11/24 17:44:21 petere Exp $

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
	cd sgml && $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm

programmer.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml programmer.html
	cd sgml && $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm -C ../graphics catalogs.gif connections.gif

tutorial.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml tutorial.html
	cd sgml && $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm -C ../graphics clientserver.gif

postgres.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml postgres.html
	cd sgml && $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm -C ../graphics catalogs.gif clientserver.gif connections.gif

man.tar:
	$(MAKE) -C sgml man
	$(TAR) -cf $@ -C sgml man1 manl

# Generic production rules

# Compressed file

%.$(ZIPSUFFIX):	%
	$(ZIP) -f $<
