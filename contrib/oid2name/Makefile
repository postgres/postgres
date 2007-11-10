# $PostgreSQL: pgsql/contrib/oid2name/Makefile,v 1.10 2007/11/10 23:59:51 momjian Exp $

PROGRAM = oid2name
OBJS	= oid2name.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/oid2name
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
