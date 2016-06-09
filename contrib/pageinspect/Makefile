# contrib/pageinspect/Makefile

MODULE_big	= pageinspect
OBJS		= rawpage.o heapfuncs.o btreefuncs.o fsmfuncs.o \
		  brinfuncs.o ginfuncs.o $(WIN32RES)

EXTENSION = pageinspect
DATA = pageinspect--1.5.sql pageinspect--1.4--1.5.sql \
	pageinspect--1.3--1.4.sql pageinspect--1.2--1.3.sql \
	pageinspect--1.1--1.2.sql pageinspect--1.0--1.1.sql \
	pageinspect--unpackaged--1.0.sql
PGFILEDESC = "pageinspect - functions to inspect contents of database pages"

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
