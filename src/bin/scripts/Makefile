#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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

PROGRAMS = createdb createuser dropdb dropuser clusterdb vacuumdb reindexdb pg_isready

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

all: $(PROGRAMS)

createdb: createdb.o common.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils
createuser: createuser.o common.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils
dropdb: dropdb.o common.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils
dropuser: dropuser.o common.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils
clusterdb: clusterdb.o common.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils
vacuumdb: vacuumdb.o common.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils
reindexdb: reindexdb.o common.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils
pg_isready: pg_isready.o common.o $(WIN32RES) | submake-libpq submake-libpgport submake-libpgfeutils

install: all installdirs
	$(INSTALL_PROGRAM) createdb$(X)   '$(DESTDIR)$(bindir)'/createdb$(X)
	$(INSTALL_PROGRAM) dropdb$(X)     '$(DESTDIR)$(bindir)'/dropdb$(X)
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

clean distclean:
	rm -f $(addsuffix $(X), $(PROGRAMS)) $(addsuffix .o, $(PROGRAMS))
	rm -f common.o $(WIN32RES)
	rm -rf tmp_check

export with_icu

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
