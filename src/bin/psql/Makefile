#-------------------------------------------------------------------------
#
# Makefile for src/bin/psql
#
# Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/psql/Makefile,v 1.54 2005/05/24 16:45:23 tgl Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "psql - the PostgreSQL interactive terminal"
PGAPPICON=win32
subdir = src/bin/psql
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

REFDOCDIR= $(top_srcdir)/doc/src/sgml/ref

override CPPFLAGS := -DFRONTEND -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS=	command.o common.o help.o input.o stringutils.o mainloop.o copy.o \
	startup.o prompt.o variables.o large_obj.o print.o describe.o \
	psqlscan.o tab-complete.o mbprint.o $(WIN32RES)

FLEXFLAGS = -Cfe


all: submake-libpq submake-libpgport psql

psql: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

help.o: $(srcdir)/sql_help.h

ifdef PERL
$(srcdir)/sql_help.h: create_help.pl $(wildcard $(REFDOCDIR)/*.sgml)
	$(PERL) $< $(REFDOCDIR) $@
else
$(srcdir)/sql_help.h:
	@echo "*** Perl is needed to build psql help."
endif

$(srcdir)/psqlscan.c: psqlscan.l
ifdef FLEX
	$(FLEX) $(FLEXFLAGS) -o'$@' $<
else
	@$(missing) flex $< $@
endif

distprep: $(srcdir)/sql_help.h $(srcdir)/psqlscan.c

install: all installdirs
	$(INSTALL_PROGRAM) psql$(X) $(DESTDIR)$(bindir)/psql$(X)
	$(INSTALL_DATA) $(srcdir)/psqlrc.sample $(DESTDIR)$(datadir)/psqlrc.sample

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/psql$(X)

# psqlscan.c is in the distribution tarball, so is not cleaned here
clean distclean:
	rm -f psql$(X) $(OBJS)

maintainer-clean: distclean
	rm -f $(srcdir)/sql_help.h $(srcdir)/psqlscan.c
