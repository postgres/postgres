# contrib/tablefunc/Makefile

MODULES = tablefunc

EXTENSION = tablefunc
DATA = tablefunc--1.0.sql tablefunc--unpackaged--1.0.sql

REGRESS = tablefunc

LDFLAGS_SL += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/tablefunc
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
