# $PostgreSQL: pgsql/contrib/spi/Makefile,v 1.23 2003/11/29 19:51:35 pgsql Exp $

subdir = contrib/spi
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULES = autoinc insert_username moddatetime refint timetravel
DATA_built = $(addsuffix .sql, $(MODULES))
DOCS	= README.spi $(addsuffix .example, $(MODULES))

# this is needed for the regression tests;
# comment out if you want a quieter refint package for other uses
PG_CPPFLAGS = -DREFINT_VERBOSE

include $(top_srcdir)/contrib/contrib-global.mk
