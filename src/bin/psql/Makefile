#-------------------------------------------------------------------------
#
# Makefile for src/bin/psql
#
# Copyright (c) 1994, Regents of the University of California
#
# $Header: /cvsroot/pgsql/src/bin/psql/Makefile,v 1.21 2000/06/27 00:31:41 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/bin/psql
top_builddir = ../../..
include ../../Makefile.global

REFDOCDIR= $(top_srcdir)/doc/src/sgml/ref

CFLAGS+= -I$(LIBPQDIR)

OBJS=command.o common.o help.o input.o stringutils.o mainloop.o \
	copy.o startup.o prompt.o variables.o large_obj.o print.o describe.o \
	tab-complete.o

all: submake psql$(X)

ifdef STRDUP
OBJS+=$(top_builddir)/src/utils/strdup.o

$(top_builddir)/src/utils/strdup.o:
	$(MAKE) -C $(top_builddir)/src/utils strdup.o
endif

# Move these to the utils directory?

ifdef STRERROR
OBJS+=$(top_builddir)/src/backend/port/strerror.o

$(top_builddir)/src/backend/port/strerror.o:
	$(MAKE) -C $(top_builddir)/src/backend/port strerror.o
endif

ifdef SNPRINTF
OBJS+=$(top_builddir)/src/backend/port/snprintf.o

$(top_builddir)/src/backend/port/snprintf.o:
	$(MAKE) -C $(top_builddir)/src/backend/port snprintf.o
endif

# End of hacks for picking up backend 'port' modules

psql$(X): $(OBJS) $(LIBPQDIR)/libpq.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBPQ) $(LDFLAGS)

help.o: $(srcdir)/sql_help.h

ifdef PERL
$(srcdir)/sql_help.h: create_help.pl $(wildcard $(REFDOCDIR)/*.sgml)
	$(PERL) $< $(REFDOCDIR) $@
else
$(srcdir)/sql_help.h:
	@echo "*** Perl is needed to build psql help."
endif

.PHONY: submake

submake:
	$(MAKE) -C $(LIBPQDIR) libpq.a

install: all installdirs
	$(INSTALL_PROGRAM) psql$(X) $(bindir)/psql$(X)

installdirs:
	$(mkinstalldirs) $(bindir)

uninstall:
	rm -f $(bindir)/psql$(X)

depend dep:
	$(CC) -MM -MG $(CFLAGS) *.c >depend

clean distclean:
	rm -f psql$(X) $(OBJS)

maintainer-clean: distclean
	rm -f sql_help.h

ifeq (depend,$(wildcard depend))
include depend
endif
