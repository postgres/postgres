#
# Makefile for src/bin/pg_passwd
#

subdir = src/bin/pg_passwd
top_builddir = ../../..
include ../../Makefile.global

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
