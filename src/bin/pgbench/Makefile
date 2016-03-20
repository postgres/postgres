# src/bin/pgbench/Makefile

PGFILEDESC = "pgbench - a simple program for running benchmark tests"
PGAPPICON = win32

subdir = src/bin/pgbench
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = pgbench.o exprparse.o psqlscan.o $(WIN32RES)

override CPPFLAGS := -I. -I$(srcdir) -I$(libpq_srcdir) \
	-I$(top_srcdir)/src/bin/psql $(CPPFLAGS)

ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
endif


all: pgbench

pgbench: $(OBJS) | submake-libpq submake-libpgport
	$(CC) $(CFLAGS) $^ $(libpq_pgport) $(PTHREAD_LIBS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

# exprscan is compiled as part of exprparse
exprparse.o: exprscan.c

# we import psqlscan.o as-is from psql
submake-psqlscan:
	$(MAKE) -C $(top_builddir)/src/bin/psql psqlscan.o

psqlscan.o: | submake-psqlscan
	rm -f $@ && $(LN_S) $(top_builddir)/src/bin/psql/psqlscan.o .

.PHONY: submake-psqlscan

distprep: exprparse.c exprscan.c

install: all installdirs
	$(INSTALL_PROGRAM) pgbench$(X) '$(DESTDIR)$(bindir)/pgbench$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pgbench$(X)'

clean distclean:
	rm -f pgbench$(X) $(OBJS)
	rm -rf tmp_check

maintainer-clean: distclean
	rm -f exprparse.c exprscan.c

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
