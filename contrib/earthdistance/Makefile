# contrib/earthdistance/Makefile

MODULES = earthdistance

EXTENSION = earthdistance
DATA = earthdistance--1.0.sql earthdistance--unpackaged--1.0.sql

REGRESS = earthdistance
REGRESS_OPTS = --extra-install=contrib/cube

LDFLAGS_SL += $(filter -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/earthdistance
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
