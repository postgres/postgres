# contrib/amcheck/Makefile

MODULE_big	= amcheck
OBJS		= verify_nbtree.o $(WIN32RES)

EXTENSION = amcheck
DATA = amcheck--1.0.sql
PGFILEDESC = "amcheck - function for verifying relation integrity"

REGRESS = check check_btree

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/amcheck
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
