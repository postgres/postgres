# contrib/hstore/Makefile

MODULE_big = hstore
OBJS = \
	$(WIN32RES) \
	hstore_compat.o \
	hstore_gin.o \
	hstore_gist.o \
	hstore_io.o \
	hstore_op.o

EXTENSION = hstore
DATA = hstore--1.4.sql \
	hstore--1.6--1.7.sql \
	hstore--1.5--1.6.sql \
	hstore--1.4--1.5.sql \
	hstore--1.3--1.4.sql hstore--1.2--1.3.sql \
	hstore--1.1--1.2.sql hstore--1.0--1.1.sql
PGFILEDESC = "hstore - key/value pair data type"

HEADERS = hstore.h

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
