#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/transam
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/transam/Makefile,v 1.12 2000/08/31 16:09:46 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/access/transam
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = transam.o transsup.o varsup.o xact.o xid.o xlog.o rmgr.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

# ensure that version checks in xlog.c get recompiled when config.h or
# catversion.h changes, even if "make depend" hasn't been done.
xlog.o: xlog.c $(top_builddir)/src/include/config.h $(top_srcdir)/src/include/catalog/catversion.h

ifeq (depend,$(wildcard depend))
include depend
endif

