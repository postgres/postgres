# $PostgreSQL: pgsql/contrib/dbase/Makefile,v 1.5 2003/11/29 19:51:22 pgsql Exp $

subdir = contrib/dbase
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = dbf2pg
OBJS	= dbf.o dbf2pg.o endian.o
PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

# Uncomment this to provide charset translation
#PG_CPPFLAGS += -DHAVE_ICONV_H
# You might need to uncomment this too, if libiconv is a separate
# library on your platform
#PG_LIBS += -liconv

DOCS = README.dbf2pg
MAN = dbf2pg.1			# XXX not implemented

include $(top_srcdir)/contrib/contrib-global.mk
