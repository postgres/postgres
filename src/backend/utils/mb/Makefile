#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/mb
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/utils/mb/Makefile,v 1.19 2002/08/06 05:40:45 ishii Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/mb
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = encnames.o conv.o mbutils.o wchar.o wstrcmp.o wstrncmp.o
DIRS = conversion_procs

all install installdirs uninstal distprep: SUBSYS.o
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@ || exit; done

clean distclean maintainer-clean:
	rm -f SUBSYS.o $(OBJS)
	@for dir in $(DIRS); do $(MAKE) -C $$dir $@; done

SUBSYS.o: $(OBJS)
	@for dir in $(DIRS); do $(MAKE) -C $$dir all || exit; done
	$(LD) $(LDREL) $(LDOUT) $@ $^

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

ifeq (depend,$(wildcard depend))
include depend
endif
