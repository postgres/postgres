# $PostgreSQL: pgsql/contrib/dblink/Makefile,v 1.12 2006/09/10 21:38:26 tgl Exp $

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

# Apparently AIX requires that we mention everything libpq depends on :-(
SHLIB_LINK += $(filter -lintl -lssl -lcrypto, $(LIBS))
