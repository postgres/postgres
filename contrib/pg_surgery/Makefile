# contrib/pg_surgery/Makefile

MODULE_big = pg_surgery
OBJS = \
	$(WIN32RES) \
	heap_surgery.o

EXTENSION = pg_surgery
DATA = pg_surgery--1.0.sql
PGFILEDESC = "pg_surgery - perform surgery on a damaged relation"

REGRESS = heap_surgery

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_surgery
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
