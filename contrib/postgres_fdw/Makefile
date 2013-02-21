# contrib/postgres_fdw/Makefile

MODULE_big = postgres_fdw
OBJS = postgres_fdw.o option.o deparse.o connection.o

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK = $(libpq)
SHLIB_PREREQS = submake-libpq

EXTENSION = postgres_fdw
DATA = postgres_fdw--1.0.sql

REGRESS = postgres_fdw

# the db name is hard-coded in the tests
override USE_MODULE_DB =

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/postgres_fdw
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
