#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/buffer
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/buffer/Makefile,v 1.16 2001/09/27 19:10:02 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/buffer
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = buf_table.o buf_init.o bufmgr.o freelist.o localbuf.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif
