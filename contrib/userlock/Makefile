# $PostgreSQL: pgsql/contrib/userlock/Makefile,v 1.19 2005/09/27 17:13:13 tgl Exp $

MODULES = user_locks
DATA_built = user_locks.sql
DOCS = README.user_locks

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/userlock
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
