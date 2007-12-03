# $PostgreSQL: pgsql/contrib/spi/Makefile,v 1.28 2007/12/03 04:22:54 tgl Exp $

MODULES = autoinc insert_username moddatetime refint timetravel
DATA_built = $(addsuffix .sql, $(MODULES))
DOCS = $(addsuffix .example, $(MODULES))

# this is needed for the regression tests;
# comment out if you want a quieter refint package for other uses
PG_CPPFLAGS = -DREFINT_VERBOSE

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/spi
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += -L$(top_builddir)/src/port -lpgport
