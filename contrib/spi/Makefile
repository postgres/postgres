# $Header: /cvsroot/pgsql/contrib/spi/Makefile,v 1.21 2001/09/06 10:49:30 petere Exp $

subdir = contrib/spi
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

MODULES = autoinc insert_username moddatetime refint timetravel
DATA_built = $(addsuffix .sql, $(MODULES))
DOCS	= README.spi $(addsuffix .example, $(MODULES))

ifdef REFINT_VERBOSE
PG_CPPFLAGS = -DREFINT_VERBOSE
endif

include $(top_srcdir)/contrib/contrib-global.mk
