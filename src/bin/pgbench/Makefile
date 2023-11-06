# src/bin/pgbench/Makefile

PGFILEDESC = "pgbench - a simple program for running benchmark tests"
PGAPPICON = win32

subdir = src/bin/pgbench
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	exprparse.o \
	exprscan.o \
	pgbench.o

override CPPFLAGS := -I. -I$(srcdir) -I$(libpq_srcdir) $(CPPFLAGS)
LDFLAGS_INTERNAL += -L$(top_builddir)/src/fe_utils -lpgfeutils $(libpq_pgport)

ifneq ($(PORTNAME), win32)
override CFLAGS += $(PTHREAD_CFLAGS)
else
override CPPFLAGS += -DFD_SETSIZE=1024
endif
LIBS += $(PTHREAD_LIBS)


all: pgbench

pgbench: $(OBJS) | submake-libpq submake-libpgport submake-libpgfeutils
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

# See notes in src/backend/parser/Makefile about the following two rules
exprparse.h: exprparse.c
	touch $@

exprparse.c: BISONFLAGS += -d

# Force these dependencies to be known even without dependency info built:
exprparse.o exprscan.o: exprparse.h

install: all installdirs
	$(INSTALL_PROGRAM) pgbench$(X) '$(DESTDIR)$(bindir)/pgbench$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pgbench$(X)'

clean distclean:
	rm -f pgbench$(X) $(OBJS)
	rm -rf tmp_check
	rm -f exprparse.h exprparse.c exprscan.c

check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
