# $PostgreSQL: pgsql/contrib/vacuumlo/Makefile,v 1.13.4.1 2005/03/25 18:18:39 momjian Exp $

PROGRAM = vacuumlo
OBJS	= vacuumlo.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

DOCS = README.vacuumlo

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/vacuumlo
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
