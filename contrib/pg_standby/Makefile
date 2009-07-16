# $PostgreSQL: pgsql/contrib/pg_standby/Makefile,v 1.5 2009/07/16 09:59:46 heikki Exp $

PROGRAM = pg_standby
OBJS	= pg_standby.o

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
