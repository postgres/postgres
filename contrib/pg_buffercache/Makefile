# $PostgreSQL: pgsql/contrib/pg_buffercache/Makefile,v 1.3 2006/02/27 12:54:39 petere Exp $

MODULE_big = pg_buffercache
OBJS	= pg_buffercache_pages.o

DATA_built = pg_buffercache.sql 
DATA = uninstall_pg_buffercache.sql 
DOCS = README.pg_buffercache

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pg_buffercache
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
