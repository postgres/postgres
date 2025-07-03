# contrib/btree_gin/Makefile

MODULE_big = btree_gin
OBJS = \
	$(WIN32RES) \
	btree_gin.o

EXTENSION = btree_gin
DATA = btree_gin--1.0.sql btree_gin--1.0--1.1.sql btree_gin--1.1--1.2.sql \
	 btree_gin--1.2--1.3.sql btree_gin--1.3--1.4.sql
PGFILEDESC = "btree_gin - B-tree equivalent GIN operator classes"

REGRESS = install_btree_gin int2 int4 int8 float4 float8 money oid \
	timestamp timestamptz time timetz date interval \
	macaddr macaddr8 inet cidr text varchar char bytea bit varbit \
	numeric enum uuid name bool bpchar

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/btree_gin
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
