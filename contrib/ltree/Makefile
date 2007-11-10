# $PostgreSQL: pgsql/contrib/ltree/Makefile,v 1.8 2007/11/10 23:59:51 momjian Exp $

PG_CPPFLAGS = -DLOWER_NODE
MODULE_big = ltree
OBJS = 	ltree_io.o ltree_op.o lquery_op.o _ltree_op.o crc32.o \
	ltxtquery_io.o ltxtquery_op.o ltree_gist.o _ltree_gist.o
DATA_built = ltree.sql
DATA = uninstall_ltree.sql
REGRESS = ltree

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/ltree
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
