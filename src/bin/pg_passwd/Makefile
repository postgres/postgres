# $Header: /cvsroot/pgsql/src/bin/pg_passwd/Attic/Makefile,v 1.10 2000/08/31 16:11:20 petere Exp $

subdir = src/bin/pg_passwd
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

all: pg_passwd$(X)

pg_passwd$(X): pg_passwd.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install: all installdirs
	$(INSTALL_PROGRAM) pg_passwd$(X) $(bindir)/pg_passwd$(X)

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/pg_passwd$(X)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean distclean maintainer-clean:
	rm -f pg_passwd$(X) pg_passwd.o

ifeq (depend,$(wildcard depend))
include depend
endif
