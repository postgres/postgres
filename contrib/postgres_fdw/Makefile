# contrib/postgres_fdw/Makefile

MODULE_big = postgres_fdw
OBJS = \
	$(WIN32RES) \
	connection.o \
	deparse.o \
	option.o \
	postgres_fdw.o \
	shippable.o
PGFILEDESC = "postgres_fdw - foreign data wrapper for PostgreSQL"

PG_CPPFLAGS = -I$(libpq_srcdir)
SHLIB_LINK_INTERNAL = $(libpq)

EXTENSION = postgres_fdw
DATA = postgres_fdw--1.0.sql

REGRESS = postgres_fdw

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
SHLIB_PREREQS = submake-libpq
subdir = contrib/postgres_fdw
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
