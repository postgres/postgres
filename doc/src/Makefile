# Postgres documentation makefile
# Thomas Lockhart

# Not yet generated from configure, so use relative path names for now...
PGDOCS= ..
SRCDIR= ../../src

TAR= tar
ZIP= gzip
TAREXCLUDE= --exclude=Makefile --exclude='*.sgml' --exclude=ref

# Pick up Makefile.global from the source area
# This is the only resource from the code source area and is optional

ifneq ($(wildcard $(SRCDIR)/Makefile.global), )
include $(SRCDIR)/Makefile.global
endif

SRC= admin postgres programmer tutorial user

TARGETS= $(SRC:%=%.tar.gz)

.PRECIOUS:
.PHONY: install all sources clean distclean

install::
	$(MAKE) all
	(mv -f *.gz ..)

clean::
	$(MAKE) -C sgml clean

distclean::
	$(MAKE) -C sgml distclean

all:: $(TARGETS)

sources::
	$(MAKE) sources.tar.gz

sources.tar:
	($(TAR) -cf $@ sgml graphics)

admin.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml admin.html
	(cd sgml; $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm -C ../graphics layout.gif)

installation.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml installation.html
	(cd sgml; $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm)

postgres.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml postgres.html
	(cd sgml; $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm -C ../graphics catalogs.gif clientserver.gif connections.gif layout.gif)

programmer.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml programmer.html
	(cd sgml; $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm -C ../graphics catalogs.gif connections.gif)

tutorial.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml tutorial.html
	(cd sgml; $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm -C ../graphics clientserver.gif)

user.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml user.html
	(cd sgml; $(TAR) -cf ../$@ $(TAREXCLUDE) *.html *.htm)

man.tar:
	$(MAKE) -C sgml man
	$(TAR) -cf $@ -C sgml man1 manl

# Generic production rules

# Compressed file

%.gz:	%
	($(ZIP) -f $<)

