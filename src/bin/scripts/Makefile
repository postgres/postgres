#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.24 2003/09/12 02:40:10 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/scripts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PROGRAMS = createdb createlang createuser dropdb droplang dropuser clusterdb vacuumdb

override CPPFLAGS := -I$(top_srcdir)/src/bin/pg_dump -I$(top_srcdir)/src/bin/psql -I$(libpq_srcdir) $(CPPFLAGS)

all: submake-libpq submake-backend $(PROGRAMS)

%: %.o
	$(CC) $(CFLAGS) $^ $(libpq) $(LDFLAGS) $(LIBS) -o $@

createdb: createdb.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
createlang: createlang.o common.o print.o mbprint.o
createuser: createuser.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
dropdb: dropdb.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
droplang: droplang.o common.o print.o mbprint.o
dropuser: dropuser.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
clusterdb: clusterdb.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
vacuumdb: vacuumdb.o common.o

dumputils.c: % : $(top_srcdir)/src/bin/pg_dump/%
	rm -f $@ && $(LN_S) $< .

print.c mbprint.c : % : $(top_srcdir)/src/bin/psql/%
	rm -f $@ && $(LN_S) $< .

.PHONY: submake-backend
submake-backend:
	$(MAKE) -C $(top_builddir)/src/backend/parser keywords.o


install: all installdirs
	$(INSTALL_PROGRAM) createdb$(X)   $(DESTDIR)$(bindir)/createdb$(X)
	$(INSTALL_PROGRAM) dropdb$(X)     $(DESTDIR)$(bindir)/dropdb$(X)
	$(INSTALL_PROGRAM) createlang$(X) $(DESTDIR)$(bindir)/createlang$(X)
	$(INSTALL_PROGRAM) droplang$(X)   $(DESTDIR)$(bindir)/droplang$(X)
	$(INSTALL_PROGRAM) createuser$(X) $(DESTDIR)$(bindir)/createuser$(X)
	$(INSTALL_PROGRAM) dropuser$(X)   $(DESTDIR)$(bindir)/dropuser$(X)
	$(INSTALL_PROGRAM) clusterdb$(X)  $(DESTDIR)$(bindir)/clusterdb$(X)
	$(INSTALL_PROGRAM) vacuumdb$(X)   $(DESTDIR)$(bindir)/vacuumdb$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(bindir)/, $(addsuffix $(X), $(PROGRAMS)))


clean distclean maintainer-clean:
	rm -f $(addsuffix $(X), $(PROGRAMS)) $(addsuffix .o, $(PROGRAMS)) common.o dumputils.o print.o mbprint.o dumputils.c print.c mbprint.c
