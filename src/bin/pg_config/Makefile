#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_config
#
# Copyright (c) 1998-2005, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/bin/pg_config/Makefile,v 1.14 2005/01/20 22:54:57 neilc Exp $
#
#-------------------------------------------------------------------------

PGFILEDESC = "pg_config - report configuration information"
subdir = src/bin/pg_config
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS=   pg_config.o $(WIN32RES)

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) -DVAL_CONFIGURE="\"$(configure_args)\"" $(CPPFLAGS)

all: submake-libpgport pg_config

pg_config: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_SCRIPT) pg_config$(X) $(DESTDIR)$(bindir)/pg_config$(X)

installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_config$(X)

clean distclean maintainer-clean:
	rm -f pg_config$(X) $(OBJS)
