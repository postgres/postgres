# $PostgreSQL: pgsql/contrib/oid2name/Makefile,v 1.8 2005/09/27 17:13:06 tgl Exp $

PROGRAM = oid2name
OBJS	= oid2name.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

DOCS = README.oid2name

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/oid2name
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
