# contrib/pgbench/Makefile

PGFILEDESC = "pgbench - a simple program for running benchmark tests"
PGAPPICON = win32

PROGRAM = pgbench
OBJS	= pgbench.o exprparse.o $(WIN32RES)

EXTRA_CLEAN	= exprparse.c exprscan.c

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

# There is no correct way to write a rule that generates two files.
# Rules with two targets don't have that meaning, they are merely
# shorthand for two otherwise separate rules.  To be safe for parallel
# make, we must chain the dependencies like this.  The semicolon is
# important; otherwise, make will choose the built-in rule.

exprparse.h: exprparse.c ;

# exprscan is compiled as part of exprparse
exprparse.o: exprscan.c
