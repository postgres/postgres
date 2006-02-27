# $PostgreSQL: pgsql/contrib/userlock/Makefile,v 1.20 2006/02/27 12:54:39 petere Exp $

MODULES = user_locks
DATA_built = user_locks.sql
DATA = uninstall_user_locks.sql
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
