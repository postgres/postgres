# contrib/oauth_provider/Makefile

MODULE_big = oauth_provider
OBJS = oauth_provider.o

EXTENSION = oauth_provider
PGFILEDESC = "oauth_provider - validate provider specific token"
DATA = oauth_provider--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/oauth_provider
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
