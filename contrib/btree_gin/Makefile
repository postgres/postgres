# $PostgreSQL: pgsql/contrib/btree_gin/Makefile,v 1.1 2009/03/25 23:20:01 tgl Exp $

MODULE_big = btree_gin
OBJS = btree_gin.o 

DATA_built = btree_gin.sql
DATA = uninstall_btree_gin.sql
REGRESS = install_btree_gin int2 int4 int8 float4 float8 money oid \
	timestamp timestamptz time timetz date interval \
	macaddr inet cidr text varchar char bytea bit varbit \
	numeric

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
