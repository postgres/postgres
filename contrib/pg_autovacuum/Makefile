
subdir = contrib/pg_autovacuum
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

PROGRAM = pg_autovacuum
OBJS	= pg_autovacuum.o

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq)

DOCS = README.pg_autovacuum

include $(top_srcdir)/contrib/contrib-global.mk
