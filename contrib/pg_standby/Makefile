# $PostgreSQL: pgsql/contrib/pg_standby/Makefile,v 1.2 2007/02/09 17:04:00 petere Exp $

PROGRAM = pg_standby
OBJS	= pg_standby.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

DOCS = README.pg_standby

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pg_standby
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
