# $PostgreSQL: pgsql/contrib/fuzzystrmatch/Makefile,v 1.7 2006/02/27 12:54:38 petere Exp $

MODULE_big = fuzzystrmatch
SRCS += fuzzystrmatch.c dmetaphone.c
OBJS = $(SRCS:.c=.o)
DATA_built = fuzzystrmatch.sql
DATA = uninstall_fuzzystrmatch.sql
DOCS = README.fuzzystrmatch README.soundex

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/fuzzystrmatch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
