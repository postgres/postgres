# $Header: /cvsroot/pgsql/contrib/pg_resetxlog/Attic/Makefile,v 1.4 2002/07/18 03:59:49 momjian Exp $

subdir = contrib/pg_resetxlog
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = pg_resetxlog
OBJS	= pg_resetxlog.o pg_crc.o $(notdir $(SNPRINTF))

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

ifdef SNPRINTF
$(basename $(notdir $(SNPRINTF))).c: $(basename $(SNPRINTF)).c
	rm -f $@ && $(LN_S) $< .
endif

EXTRA_CLEAN = pg_crc.c snprintf.c

DOCS = README.pg_resetxlog

include $(top_srcdir)/contrib/contrib-global.mk
