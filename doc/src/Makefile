# Postgres documentation makefile
# Thomas Lockhart

PGDOCS= ..
SRCDIR= ../../src

TAR= tar
ZIP= gzip
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
	$(MAKE) sources.tar.gz

sources.tar:
	($(TAR) -cf $@ sgml graphics)

admin.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml admin.html
	($(TAR) -cf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics layout.gif)

postgres.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml postgres.html
	($(TAR) -cf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics catalogs.gif connections.gif layout.gif)

programmer.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml programmer.html
	($(TAR) -cf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics catalogs.gif connections.gif)

tutorial.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml tutorial.html
	($(TAR) -cf $@ $(TAREXCLUDE) -C sgml . -C .. -C graphics clientserver.gif)

user.tar:
	$(MAKE) -C sgml clean
	$(MAKE) -C sgml user.html
	($(TAR) -cf $@ $(TAREXCLUDE) -C sgml .)

# Generic production rules

# Compressed file

%.gz:	%
	($(ZIP) -f $<)

