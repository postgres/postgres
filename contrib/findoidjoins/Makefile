# $PostgreSQL: pgsql/contrib/findoidjoins/Makefile,v 1.16.4.1 2005/03/25 18:18:37 momjian Exp $

PROGRAM = findoidjoins
OBJS	= findoidjoins.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

SCRIPTS = make_oidjoins_check
DOCS = README.findoidjoins

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/findoidjoins
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
