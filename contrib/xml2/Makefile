# This makefile will build the new XML and XSLT routines.
subdir = contrib/xml
top_builddir = ../../
include $(top_builddir)/src/Makefile.global

MODULE_big = pgxml

# Remove xslt_proc.o from the following line if you don't have libxslt
OBJS = xpath.o xslt_proc.o

# Remove -lxslt from the following line if you don't have libxslt.
SHLIB_LINK = -lxml2 -lxslt

DATA_built = pgxml.sql
DOCS = README.pgxml

include $(top_builddir)contrib/contrib-global.mk

