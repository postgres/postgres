#
# $Header: /cvsroot/pgsql/contrib/pg_resetxlog/Attic/Makefile,v 1.2 2001/04/03 19:01:57 tgl Exp $
#

subdir = contrib/pg_resetxlog
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

OBJS	= pg_resetxlog.o pg_crc.o $(SNPRINTF)

all: pg_resetxlog

pg_resetxlog: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

pg_crc.c: $(top_srcdir)/src/backend/utils/hash/pg_crc.c
	rm -f $@ && $(LN_S) $< .

# this only gets done if configure finds system doesn't have snprintf()
snprintf.c: $(top_srcdir)/src/backend/port/snprintf.c
	rm -f $@ && $(LN_S) $< .

install: all installdirs
	$(INSTALL_PROGRAM) pg_resetxlog$(X)	$(bindir)
	$(INSTALL_DATA) README.pg_resetxlog	$(docdir)/contrib

installdirs:
	$(mkinstalldirs) $(bindir) $(docdir)/contrib

uninstall:
	rm -f $(bindir)/pg_resetxlog$(X) $(docdir)/contrib/README.pg_resetxlog

clean distclean maintainer-clean:
	rm -f pg_resetxlog$(X) $(OBJS) pg_crc.c snprintf.c

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c > depend

ifeq (depend,$(wildcard depend))
include depend
endif
