#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/lmgr
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/storage/lmgr/Makefile,v 1.17 2001/09/29 04:02:24 tgl Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/lmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = lmgr.o lock.o proc.o deadlock.o lwlock.o spin.o s_lock.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

s_lock_test: s_lock.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DS_LOCK_TEST=1 s_lock.c -o s_lock_test
	./s_lock_test

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) s_lock_test

ifeq (depend,$(wildcard depend))
include depend
endif
