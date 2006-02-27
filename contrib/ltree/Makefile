PG_CPPFLAGS = -DLOWER_NODE
MODULE_big = ltree
OBJS = 	ltree_io.o ltree_op.o lquery_op.o _ltree_op.o crc32.o \
	ltxtquery_io.o ltxtquery_op.o ltree_gist.o _ltree_gist.o
DATA_built = ltree.sql
DATA = uninstall_ltree.sql
DOCS = README.ltree
REGRESS = ltree

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/ltree
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
