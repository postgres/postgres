PROGRAM = pg_autovacuum
OBJS	= pg_autovacuum.o dllist.o

PG_CPPFLAGS = -I$(libpq_srcdir) -DFRONTEND
PG_LIBS = $(libpq_pgport)

DOCS = README.pg_autovacuum

EXTRA_CLEAN = dllist.c

ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pg_autovacuum
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

dllist.c: $(top_srcdir)/src/backend/lib/dllist.c
	rm -f $@ && $(LN_S) $< .
