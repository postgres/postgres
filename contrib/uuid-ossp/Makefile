# $PostgreSQL: pgsql/contrib/uuid-ossp/Makefile,v 1.3 2007/11/10 23:59:51 momjian Exp $

MODULE_big = uuid-ossp
OBJS = uuid-ossp.o
DATA_built = uuid-ossp.sql
DATA = uninstall_uuid-ossp.sql

SHLIB_LINK += -lossp-uuid

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/uuid-ossp
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
