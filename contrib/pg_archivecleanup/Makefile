# $PostgreSQL: pgsql/contrib/pg_archivecleanup/Makefile,v 1.1 2010/06/14 16:19:24 sriggs Exp $

PGFILEDESC = "pg_archivecleanup - cleans archive when used with streaming replication"
PGAPPICON=win32

PROGRAM = pg_archivecleanup
OBJS	= pg_archivecleanup.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_archivecleanup
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
