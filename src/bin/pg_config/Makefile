#-------------------------------------------------------------------------
#
# Makefile for src/bin/pg_controldata
#
# Copyright (c) 1998-2002, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/bin/pg_config/Makefile,v 1.10 2004/10/04 18:05:54 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/pg_config
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS=   pg_config.o

override CPPFLAGS := -DFRONTEND -I$(libpq_srcdir) -DVAL_CONFIGURE="\"$(configure_args)\"" $(CPPFLAGS)

all: submake-libpgport pg_config

pg_config: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_SCRIPT) pg_config$(X) $(DESTDIR)$(bindir)/pg_config$(X)
  
installdirs:
	$(mkinstalldirs) $(DESTDIR)$(bindir)

uninstall:
	rm -f $(DESTDIR)$(bindir)/pg_config

clean distclean maintainer-clean:
	rm -f pg_config$(X) $(OBJS)
