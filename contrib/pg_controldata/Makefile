# $Header: /cvsroot/pgsql/contrib/pg_controldata/Attic/Makefile,v 1.8 2002/07/27 20:10:03 petere Exp $

subdir = contrib/pg_controldata
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

all: submake-libpgport

PROGRAM = pg_controldata
OBJS	= pg_controldata.o pg_crc.o

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

EXTRA_CLEAN = pg_crc.c

DOCS = README.pg_controldata

include $(top_srcdir)/contrib/contrib-global.mk
