# contrib/pageinspect/Makefile

MODULE_big	= pageinspect
OBJS		= rawpage.o heapfuncs.o btreefuncs.o fsmfuncs.o

EXTENSION = pageinspect
DATA = pageinspect--1.2.sql pageinspect--1.0--1.1.sql \
	pageinspect--1.1--1.2.sql pageinspect--unpackaged--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pageinspect
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
