# $PostgreSQL: pgsql/contrib/xml/Makefile,v 1.4 2003/11/29 19:51:36 pgsql Exp $

subdir = contrib/xml
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULE_big = pgxml_dom
OBJS = pgxml_dom.o
SHLIB_LINK = -lxml2
DATA_built = pgxml_dom.sql
DOCS = README.pgxml

include $(top_srcdir)/contrib/contrib-global.mk
