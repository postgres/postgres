#-------------------------------------------------------------------------
#
# Makefile for src/bin/psql
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/psql/Makefile,v 1.34 2002/07/15 21:34:05 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/psql
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

REFDOCDIR= $(top_srcdir)/doc/src/sgml/ref

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS=command.o common.o help.o input.o stringutils.o mainloop.o copy.o \
	startup.o prompt.o variables.o large_obj.o print.o describe.o \
	sprompt.o tab-complete.o mbprint.o $(SNPRINTF) $(STRDUP) \
	$(STRERROR) $(STRTOUL)

all: submake psql

# End of hacks for picking up backend 'port' modules

psql: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

help.o: $(srcdir)/sql_help.h

ifdef PERL
$(srcdir)/sql_help.h: create_help.pl $(wildcard $(REFDOCDIR)/*.sgml)
	$(PERL) $< $(REFDOCDIR) $@
else
$(srcdir)/sql_help.h:
	@echo "*** Perl is needed to build psql help."
endif

.PHONY: submake

submake:
	$(MAKE) -C $(libpq_builddir) all

distprep: $(srcdir)/sql_help.h

install: all installdirs
	$(INSTALL_PROGRAM) psql$(X) $(DESTDIR)$(bindir)/psql$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/psql$(X)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c >depend

clean distclean:
	rm -f psql$(X) $(OBJS)

maintainer-clean: distclean
	rm -f $(srcdir)/sql_help.h

ifeq (depend,$(wildcard depend))
include depend
endif
