# Postgres documentation makefile
# Thomas Lockhart

PGDOCS= ..
SRCDIR= ../../src

TAR= tar
TAREXCLUDE= --exclude=Makefile --exclude='*.sgml'

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
	($(TAR) zcf sources.tar.gz sgml graphics)

admin.tar.gz:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml admin.html
	($(TAR) zcf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics layout.gif)

postgres.tar.gz:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml postgres.html
	($(TAR) zcf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics catalogs.gif connections.gif layout.gif)

programmer.tar.gz:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml programmer.html
	($(TAR) zcf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics catalogs.gif connections.gif)

tutorial.tar.gz:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml tutorial.html
	($(TAR) zcf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics clientserver.gif)

user.tar.gz:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml user.html
	($(TAR) zcf $@ $(TAREXCLUDE) -C sgml .)

# Generic production rules

# Compressed file

%.gz:	%
	(gzip -f $<)

