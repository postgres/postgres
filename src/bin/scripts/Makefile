#-------------------------------------------------------------------------
#
# Makefile for src/bin/scripts
#
# Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/scripts/Makefile,v 1.19 2003/03/18 22:19:46 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/scripts
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SCRIPTS := vacuumdb clusterdb
PROGRAMS = createdb createlang createuser dropdb droplang dropuser

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)


all: submake-libpq submake-backend $(PROGRAMS)

%: %.o
	$(CC) $(CFLAGS) $^ $(libpq) $(LDFLAGS) $(LIBS) -o $@

createdb: createdb.o common.o dumputils.o sprompt.o $(top_builddir)/src/backend/parser/keywords.o
createlang: createlang.o common.o sprompt.o print.o mbprint.o
createuser: createuser.o common.o dumputils.o sprompt.o $(top_builddir)/src/backend/parser/keywords.o
dropdb: dropdb.o common.o dumputils.o sprompt.o $(top_builddir)/src/backend/parser/keywords.o
droplang: droplang.o common.o sprompt.o print.o mbprint.o
dropuser: dropuser.o common.o dumputils.o sprompt.o $(top_builddir)/src/backend/parser/keywords.o

createdb.o createuser.o dropdb.o dropuser.o dumputils.o: dumputils.h

createlang.o droplang.o: print.h
print.o: print.h mbprint.h

dumputils.c dumputils.h sprompt.c : % : $(top_srcdir)/src/bin/pg_dump/%
	rm -f $@ && $(LN_S) $< .

print.c print.h mbprint.c mbprint.h : % : $(top_srcdir)/src/bin/psql/%
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
	$(INSTALL_SCRIPT) $(srcdir)/clusterdb $(DESTDIR)$(bindir)/clusterdb
	$(INSTALL_SCRIPT) $(srcdir)/vacuumdb  $(DESTDIR)$(bindir)/vacuumdb

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(bindir)/, $(SCRIPTS) $(addsuffix $(X), $(PROGRAMS)))


clean distclean maintainer-clean:
	rm -f $(addsuffix $(X), $(PROGRAMS)) $(addsuffix .o, $(PROGRAMS)) common.o dumputils.o sprompt.o print.o mbprint.o dumputils.c dumputils.h sprompt.c print.c print.h mbprint.c mbprint.h
