#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/transam
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/access/transam/Makefile,v 1.11 2000/05/29 05:44:41 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../../..
include ../../../Makefile.global

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
xlog.o: xlog.c $(SRCDIR)/include/config.h $(SRCDIR)/include/catalog/catversion.h

ifeq (depend,$(wildcard depend))
include depend
endif

