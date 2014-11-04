# contrib/hstore/Makefile

MODULE_big = hstore
OBJS = hstore_io.o hstore_op.o hstore_gist.o hstore_gin.o hstore_compat.o \
	$(WIN32RES)

EXTENSION = hstore
DATA = hstore--1.3.sql hstore--1.2--1.3.sql \
	hstore--1.1--1.2.sql hstore--1.0--1.1.sql \
	hstore--unpackaged--1.0.sql
PGFILEDESC = "hstore - key/value pair data type"

REGRESS = hstore

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/hstore
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
