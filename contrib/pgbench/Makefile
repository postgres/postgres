#
# $Header: /cvsroot/pgsql/contrib/pgbench/Makefile,v 1.5 2000/07/09 13:13:29 petere Exp $
#

subdir = contrib/pgbench
top_builddir = ../..
include ../../src/Makefile.global

CFLAGS	+= -I$(LIBPQDIR)
LIBS	+= $(LIBPQ)

all: pgbench

install: all installdirs
	$(INSTALL_PROGRAM) pgbench$(X)	$(bindir)
	$(INSTALL_DATA) README.pgbench	$(docdir)/contrib
	$(INSTALL_DATA) README.pgbench_jis $(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/pgbench$(X) $(docdir)/contrib/README.pgbench $(docdir)/contrib/README.pgbench_jis

clean distclean maintainer-clean:
	rm -f pgbench$(X)
