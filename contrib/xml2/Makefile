# contrib/xml2/Makefile

MODULE_big = pgxml
OBJS = \
	$(WIN32RES) \
	xpath.o \
	xslt_proc.o

EXTENSION = xml2
DATA = xml2--1.1.sql xml2--1.0--1.1.sql
PGFILEDESC = "xml2 - XPath querying and XSLT"

REGRESS = xml2

SHLIB_LINK += $(filter -lxslt, $(LIBS)) -lxml2

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/xml2
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
