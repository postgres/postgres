# $PostgreSQL: pgsql/contrib/pg_trgm/Makefile,v 1.7 2007/03/14 14:15:40 teodor Exp $

MODULE_big = pg_trgm
OBJS = trgm_op.o trgm_gist.o trgm_gin.o

DATA_built = pg_trgm.sql
DATA = uninstall_pg_trgm.sql
DOCS = README.pg_trgm
REGRESS = pg_trgm


ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pg_trgm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
