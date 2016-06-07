# contrib/earthdistance/Makefile

MODULES = earthdistance

EXTENSION = earthdistance
DATA = earthdistance--1.1.sql earthdistance--1.0--1.1.sql \
	earthdistance--unpackaged--1.0.sql
PGFILEDESC = "earthdistance - calculate distances on the surface of the Earth"

REGRESS = earthdistance
EXTRA_INSTALL = contrib/cube

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
