# contrib/authn_id/Makefile

MODULE_big = authn_id
OBJS = authn_id.o

EXTENSION = authn_id
DATA = authn_id--1.0.sql
PGFILEDESC = "authn_id - information about the authenticated user"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/authn_id
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
