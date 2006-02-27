# $PostgreSQL: pgsql/contrib/dblink/Makefile,v 1.11 2006/02/27 12:54:38 petere Exp $

MODULE_big = dblink
PG_CPPFLAGS = -I$(libpq_srcdir)
OBJS	= dblink.o
SHLIB_LINK = $(libpq)

DATA_built = dblink.sql 
DATA = uninstall_dblink.sql 
DOCS = README.dblink
REGRESS = dblink


ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/dblink
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
