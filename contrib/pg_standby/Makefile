# $PostgreSQL: pgsql/contrib/pg_standby/Makefile,v 1.3 2007/06/26 22:05:02 tgl Exp $

PROGRAM = pg_standby
OBJS	= pg_standby.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

DOCS = README.pg_standby

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_standby
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
