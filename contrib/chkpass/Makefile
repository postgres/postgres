# $PostgreSQL: pgsql/contrib/chkpass/Makefile,v 1.5 2003/11/29 19:51:19 pgsql Exp $

subdir = contrib/chkpass
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = chkpass
OBJS = chkpass.o
SHLIB_LINK = $(filter -lcrypt, $(LIBS))
DATA_built = chkpass.sql
DOCS = README.chkpass

include $(top_srcdir)/contrib/contrib-global.mk
