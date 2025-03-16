# contrib/isn/Makefile

MODULES = isn

EXTENSION = isn
DATA = isn--1.0--1.1.sql isn--1.1.sql \
	isn--1.1--1.2.sql isn--1.2--1.3.sql
PGFILEDESC = "isn - data types for international product numbering standards"

# the other .h files are data tables, we don't install those
HEADERS_isn = isn.h

REGRESS = isn

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/isn
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
