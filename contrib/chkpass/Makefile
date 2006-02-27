# $PostgreSQL: pgsql/contrib/chkpass/Makefile,v 1.8 2006/02/27 12:54:38 petere Exp $

MODULE_big = chkpass
OBJS = chkpass.o
SHLIB_LINK = $(filter -lcrypt, $(LIBS))
DATA_built = chkpass.sql
DATA = uninstall_chkpass.sql
DOCS = README.chkpass

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/chkpass
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
