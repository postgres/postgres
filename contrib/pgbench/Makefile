#
# $Header: /cvsroot/pgsql/contrib/pgbench/Makefile,v 1.9 2001/02/20 19:20:27 petere Exp $
#

subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

OBJS	= pgbench.o

all: pgbench

pgbench: $(OBJS) $(libpq_builddir)/libpq.a
	$(CC) $(CFLAGS) $(OBJS) $(libpq) $(LDFLAGS) $(LIBS) -o $@

install: all installdirs
	$(INSTALL_PROGRAM) pgbench$(X)	$(bindir)
	$(INSTALL_DATA) README.pgbench	$(docdir)/contrib
	$(INSTALL_DATA) README.pgbench_jis $(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/pgbench$(X) $(docdir)/contrib/README.pgbench $(docdir)/contrib/README.pgbench_jis

clean distclean maintainer-clean:
	rm -f pgbench$(X) $(OBJS)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
