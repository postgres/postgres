# $PostgreSQL: pgsql/contrib/pg_dumplo/Makefile,v 1.14 2005/03/25 18:17:11 momjian Exp $

PROGRAM = pg_dumplo
OBJS	= main.o lo_export.o lo_import.o utils.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

DOCS = README.pg_dumplo

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pg_dumplo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
