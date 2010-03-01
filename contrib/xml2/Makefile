# This makefile will build the new XML and XSLT routines.

# This module will not work without libxml2, but it will work without libxslt.
# To build without libxslt, run "gmake USE_LIBXSLT=0", or comment out the
# following line:
USE_LIBXSLT = 1

MODULE_big = pgxml

OBJS = xpath.o xslt_proc.o

PG_CPPFLAGS := $(shell xml2-config --cflags)

ifeq ($(USE_LIBXSLT),1)
PG_CPPFLAGS += -DUSE_LIBXSLT
SHLIB_LINK = -lxslt -lxml2
else
SHLIB_LINK = -lxml2
endif

DATA_built = pgxml.sql
REGRESS = xml2
DOCS = README.xml2

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/xml2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
