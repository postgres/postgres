#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_ctl
#
# Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/pg_ctl/Makefile,v 1.14 2004/05/27 03:37:55 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_ctl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -DFRONTEND -DDEF_PGPORT=$(DEF_PGPORT) -I$(libpq_srcdir) $(CPPFLAGS)

OBJS=	pg_ctl.o exec.o

all: submake-libpq submake-libpgport pg_ctl

pg_ctl: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@$(X)

exec.c: % : $(top_srcdir)/src/port/%
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_ctl$(X) $(DESTDIR)$(bindir)/pg_ctl$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_ctl$(X)

clean distclean maintainer-clean:
	rm -f pg_ctl$(X) $(OBJS) exec.c


# ensure that changes in DEF_PGPORT propagate into object file
pg_ctl.o: pg_ctl.c $(top_builddir)/src/Makefile.global
