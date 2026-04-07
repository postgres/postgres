# contrib/pg_stash_advice/Makefile

MODULE_big = pg_stash_advice
OBJS = \
	$(WIN32RES) \
	pg_stash_advice.o \
	stashfuncs.o \
	stashpersist.o

EXTENSION = pg_stash_advice
DATA = pg_stash_advice--1.0.sql
PGFILEDESC = "pg_stash_advice - store and automatically apply plan advice"

REGRESS = pg_stash_advice pg_stash_advice_utf8
TAP_TESTS = 1
EXTRA_INSTALL = contrib/pg_plan_advice

ifdef USE_PGXS
PG_CPPFLAGS = -I$(includedir_server)/extension
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
PG_CPPFLAGS = -I$(top_srcdir)/contrib/pg_plan_advice
subdir = contrib/pg_stash_advice
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
