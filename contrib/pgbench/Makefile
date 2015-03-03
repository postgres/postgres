# contrib/pgbench/Makefile

PGFILEDESC = "pgbench - a simple program for running benchmark tests"
PGAPPICON = win32

PROGRAM = pgbench
OBJS	= pgbench.o exprparse.o $(WIN32RES)

PG_CPPFLAGS = -I$(libpq_srcdir)
PG_LIBS = $(libpq_pgport) $(PTHREAD_LIBS)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

distprep: exprparse.c exprscan.c
endif

ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
endif

# exprscan is compiled as part of exprparse
exprparse.o: exprscan.c

maintainer-clean:
	rm -f exprparse.c exprscan.c
