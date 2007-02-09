# $PostgreSQL: pgsql/contrib/xml2/Makefile,v 1.9 2007/02/09 17:04:00 petere Exp $

MODULE_big = pgxml

# Remove xslt_proc.o from the following line if you don't have libxslt
OBJS = xpath.o xslt_proc.o

# Remove -lxslt from the following line if you don't have libxslt.
SHLIB_LINK = -lxslt -lxml2

DATA_built = pgxml.sql
DATA = uninstall_pgxml.sql
DOCS = README.xml2

override CFLAGS += $(shell xml2-config --cflags)

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/xml2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
