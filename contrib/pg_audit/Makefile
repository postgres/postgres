# contrib/pg_audit/Makefile

MODULE_big = pg_audit
OBJS = pg_audit.o $(WIN32RES)

EXTENSION = pg_audit
DATA = pg_audit--1.0.sql
PGFILEDESC = "pg_audit - An audit logging extension for PostgreSQL"

REGRESS = pg_audit

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_audit
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
