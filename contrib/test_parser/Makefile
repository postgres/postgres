# $PostgreSQL: pgsql/contrib/test_parser/Makefile,v 1.1 2007/10/15 21:36:50 tgl Exp $

MODULE_big = test_parser
OBJS = test_parser.o
DATA_built = test_parser.sql
DATA = uninstall_test_parser.sql
DOCS = README.test_parser
REGRESS = test_parser

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_parser
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
