#
# $Header: /cvsroot/pgsql/contrib/pgbench/Makefile,v 1.7 2000/10/20 21:03:26 petere Exp $
#

subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -I$(libpq_srcdir)
override LIBS += $(libpq)

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
