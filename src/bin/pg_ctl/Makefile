#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_ctl
#
# Copyright (c) 1999, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/bin/pg_ctl/Makefile,v 1.13 2003/12/23 21:56:21 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_ctl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: pg_ctl

pg_ctl: pg_ctl.sh $(top_builddir)/src/Makefile.global
	sed -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's,@bindir@,$(bindir),g' \
	    -e 's,@DEF_PGPORT@,$(DEF_PGPORT),g' \
	  $< >$@
	chmod a+x $@

install: all installdirs
	$(INSTALL_SCRIPT) pg_ctl $(DESTDIR)$(bindir)/pg_ctl

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_ctl

clean distclean maintainer-clean:
	rm -f pg_ctl
