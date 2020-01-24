# contrib/adminpack/Makefile

MODULE_big = adminpack
OBJS = \
	$(WIN32RES) \
	adminpack.o
PG_CPPFLAGS = -I$(libpq_srcdir)

EXTENSION = adminpack
DATA = adminpack--1.0.sql adminpack--1.0--1.1.sql adminpack--1.1--2.0.sql\
	adminpack--2.0--2.1.sql
PGFILEDESC = "adminpack - support functions for pgAdmin"

REGRESS = adminpack

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/adminpack
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
