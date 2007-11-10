# $PostgreSQL: pgsql/contrib/pg_freespacemap/Makefile,v 1.4 2007/11/10 23:59:51 momjian Exp $

MODULE_big = pg_freespacemap
OBJS	= pg_freespacemap.o

DATA_built = pg_freespacemap.sql 
DATA = uninstall_pg_freespacemap.sql 

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_freespacemap
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
