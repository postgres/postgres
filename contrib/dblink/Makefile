# $PostgreSQL: pgsql/contrib/dblink/Makefile,v 1.15 2007/11/10 23:59:50 momjian Exp $

MODULE_big = dblink
PG_CPPFLAGS = -I$(libpq_srcdir)
OBJS	= dblink.o
SHLIB_LINK = $(libpq)

DATA_built = dblink.sql 
DATA = uninstall_dblink.sql 
REGRESS = paths dblink
REGRESS_OPTS = --dbname=$(CONTRIB_TESTDB) --dlpath=$(top_builddir)/src/test/regress \
	--create-role=dblink_regression_test
EXTRA_CLEAN = sql/paths.sql expected/paths.out


# the db name is hard-coded in the tests
override USE_MODULE_DB =

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/dblink
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
