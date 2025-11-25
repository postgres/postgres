# contrib/mssql_compat/Makefile

MODULES = mssql_compat

EXTENSION = mssql_compat
DATA = mssql_compat--1.0.sql
PGFILEDESC = "mssql_compat - SQL Server compatible date functions"

REGRESS = mssql_compat

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/mssql_compat
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

