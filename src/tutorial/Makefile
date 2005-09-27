#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tutorial
#
# By default, this builds against an existing PostgreSQL installation
# (the one identified by whichever pg_config is first in your path).
# Within a configured source tree, you can say "gmake NO_PGXS=1 all"
# to build using the surrounding source tree.
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/tutorial/Makefile,v 1.19 2005/09/27 17:13:14 tgl Exp $
#
#-------------------------------------------------------------------------

MODULES = complex funcs
DATA_built = advanced.sql basics.sql complex.sql funcs.sql syscat.sql

SHLIB_LINK = $(BE_DLLLIBS)

ifdef NO_PGXS
subdir = src/tutorial
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/src/makefiles/pgxs.mk
else
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
endif

%.sql: %.source
	rm -f $@; \
	C=`pwd`; \
	sed -e "s:_OBJWD_:$$C:g" < $< > $@
