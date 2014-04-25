#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/bin/scripts/Makefile
#
#-------------------------------------------------------------------------

PGFILEDESC = "PostgreSQL utility"
PGAPPICON=win32

subdir = src/bin/scripts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

PROGRAMS = createdb createlang createuser dropdb droplang dropuser clusterdb vacuumdb reindexdb pg_isready

override CPPFLAGS := -I$(top_srcdir)/src/bin/pg_dump -I$(top_srcdir)/src/bin/psql -I$(libpq_srcdir) $(CPPFLAGS)

all: $(PROGRAMS)

%: %.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

createdb: createdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
createlang: createlang.o common.o print.o mbprint.o | submake-libpq submake-libpgport
createuser: createuser.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
dropdb: dropdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
droplang: droplang.o common.o print.o mbprint.o | submake-libpq submake-libpgport
dropuser: dropuser.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
clusterdb: clusterdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
vacuumdb: vacuumdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
reindexdb: reindexdb.o common.o dumputils.o kwlookup.o keywords.o | submake-libpq submake-libpgport
pg_isready: pg_isready.o common.o | submake-libpq submake-libpgport

dumputils.c keywords.c: % : $(top_srcdir)/src/bin/pg_dump/%
	rm -f $@ && $(LN_S) $< .

print.c mbprint.c : % : $(top_srcdir)/src/bin/psql/%
	rm -f $@ && $(LN_S) $< .

kwlookup.c: % : $(top_srcdir)/src/backend/parser/%
	rm -f $@ && $(LN_S) $< .

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
	$(INSTALL_PROGRAM) pg_isready$(X) '$(DESTDIR)$(bindir)'/pg_isready$(X)

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(bindir)'/, $(addsuffix $(X), $(PROGRAMS)))

clean distclean maintainer-clean:
	rm -f $(addsuffix $(X), $(PROGRAMS)) $(addsuffix .o, $(PROGRAMS))
	rm -f common.o dumputils.o kwlookup.o keywords.o print.o mbprint.o $(WIN32RES)
	rm -f dumputils.c print.c mbprint.c kwlookup.c keywords.c
	rm -rf tmp_check

check: all
	$(prove_check)

installcheck:
	$(prove_installcheck)
