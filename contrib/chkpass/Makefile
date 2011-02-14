# contrib/chkpass/Makefile

MODULE_big = chkpass
OBJS = chkpass.o

EXTENSION = chkpass
DATA = chkpass--1.0.sql chkpass--unpackaged--1.0.sql

SHLIB_LINK = $(filter -lcrypt, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/chkpass
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
