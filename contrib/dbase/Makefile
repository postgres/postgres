# $PostgreSQL: pgsql/contrib/dbase/Makefile,v 1.8 2005/09/27 17:13:01 tgl Exp $

PROGRAM = dbf2pg
OBJS	= dbf.o dbf2pg.o endian.o
PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport)

# Uncomment this to provide charset translation
#PG_CPPFLAGS += -DHAVE_ICONV_H
# You might need to uncomment this too, if libiconv is a separate
# library on your platform
#PG_LIBS += -liconv

DOCS = README.dbf2pg
MAN = dbf2pg.1			# XXX not implemented


ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/dbase
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
