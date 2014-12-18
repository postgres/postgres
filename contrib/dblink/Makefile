# contrib/dblink/Makefile

MODULE_big = dblink
OBJS	= dblink.o
PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK = $(libpq)

EXTENSION = dblink
DATA = dblink--1.1.sql dblink--1.0--1.1.sql dblink--unpackaged--1.0.sql

REGRESS = paths dblink
REGRESS_OPTS = --dlpath=$(top_builddir)/src/test/regress \
	--create-role=dblink_regression_test
EXTRA_CLEAN = sql/paths.sql expected/paths.out

# the db name is hard-coded in the tests
override USE_MODULE_DB =

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
SHLIB_PREREQS = submake-libpq
subdir = contrib/dblink
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
