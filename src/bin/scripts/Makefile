#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $PostgreSQL: pgsql/src/bin/scripts/Makefile,v 1.36 2006/03/05 15:58:52 momjian Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "PostgreSQL utility"
subdir = src/bin/scripts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PROGRAMS = createdb createlang createuser dropdb droplang dropuser clusterdb vacuumdb reindexdb

override CPPFLAGS := -DFRONTEND -I$(top_srcdir)/src/bin/pg_dump -I$(top_srcdir)/src/bin/psql -I$(libpq_srcdir) $(CPPFLAGS)

all: submake-libpq submake-backend $(PROGRAMS)

%: %.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LIBS) -o $@$(X)

createdb: createdb.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
createlang: createlang.o common.o print.o mbprint.o
createuser: createuser.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
dropdb: dropdb.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
droplang: droplang.o common.o print.o mbprint.o
dropuser: dropuser.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
clusterdb: clusterdb.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o
vacuumdb: vacuumdb.o common.o
reindexdb: reindexdb.o common.o dumputils.o $(top_builddir)/src/backend/parser/keywords.o

dumputils.c: % : $(top_srcdir)/src/bin/pg_dump/%
	rm -f $@ && $(LN_S) $< .

print.c mbprint.c : % : $(top_srcdir)/src/bin/psql/%
	rm -f $@ && $(LN_S) $< .

.PHONY: submake-backend
submake-backend:
	$(MAKE) -C $(top_builddir)/src/backend/parser keywords.o


install: all installdirs
	$(INSTALL_PROGRAM) createdb$(X)   '$(DESTDIR)$(bindir)'/createdb$(X)
	$(INSTALL_PROGRAM) dropdb$(X)     '$(DESTDIR)$(bindir)'/dropdb$(X)
	$(INSTALL_PROGRAM) createlang$(X) '$(DESTDIR)$(bindir)'/createlang$(X)
	$(INSTALL_PROGRAM) droplang$(X)   '$(DESTDIR)$(bindir)'/droplang$(X)
	$(INSTALL_PROGRAM) createuser$(X) '$(DESTDIR)$(bindir)'/createuser$(X)
	$(INSTALL_PROGRAM) dropuser$(X)   '$(DESTDIR)$(bindir)'/dropuser$(X)
	$(INSTALL_PROGRAM) clusterdb$(X)  '$(DESTDIR)$(bindir)'/clusterdb$(X)
	$(INSTALL_PROGRAM) vacuumdb$(X)   '$(DESTDIR)$(bindir)'/vacuumdb$(X)
	$(INSTALL_PROGRAM) reindexdb$(X)  '$(DESTDIR)$(bindir)'/reindexdb$(X)

installdirs:
	$(mkinstalldirs) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, $(addsuffix $(X), $(PROGRAMS)))


clean distclean maintainer-clean:
	rm -f $(addsuffix $(X), $(PROGRAMS)) $(addsuffix .o, $(PROGRAMS))
	rm -f common.o dumputils.o print.o mbprint.o $(WIN32RES)
	rm -f dumputils.c print.c mbprint.c
