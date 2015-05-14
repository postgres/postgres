# pg_audit/Makefile

MODULE = pg_audit
MODULE_big = pg_audit
OBJS = pg_audit.o

EXTENSION = pg_audit
REGRESS = pg_audit
REGRESS_OPTS = --temp-config=$(top_srcdir)/contrib/pg_audit/pg_audit.conf
DATA = pg_audit--1.0.0.sql

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
