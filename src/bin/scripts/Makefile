#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS += -L$(top_builddir)/src/fe_utils -lpgfeutils -lpq

all: $(PROGRAMS)

%: %.o $(WIN32RES)
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

createdb: createdb.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
createlang: createlang.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
createuser: createuser.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
dropdb: dropdb.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
droplang: droplang.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
dropuser: dropuser.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
clusterdb: clusterdb.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
vacuumdb: vacuumdb.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
reindexdb: reindexdb.o common.o | submake-libpq submake-libpgport submake-libpgfeutils
pg_isready: pg_isready.o common.o | submake-libpq submake-libpgport submake-libpgfeutils

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
	rm -f common.o $(WIN32RES)
	rm -rf tmp_check

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
