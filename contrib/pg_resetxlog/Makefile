# $Header: /cvsroot/pgsql/contrib/pg_resetxlog/Attic/Makefile,v 1.3 2001/09/06 10:49:30 petere Exp $

subdir = contrib/pg_resetxlog
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = pg_resetxlog
OBJS	= pg_resetxlog.o pg_crc.o $(SNPRINTF)

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

# this only gets done if configure finds system doesn't have snprintf()
snprintf.c: $(top_srcdir)/src/backend/port/snprintf.c
	rm -f $@ && $(LN_S) $< .

EXTRA_CLEAN = pg_crc.c snprintf.c

DOCS = README.pg_resetxlog

include $(top_srcdir)/contrib/contrib-global.mk
