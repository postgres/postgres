#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the port-specific subsystem of the backend
#
# We have two different modes of operation: 1) put stuff specific to Port X
# in subdirectory X and have that subdirectory's make file make it all, and 
# 2) use conditional statements in the present make file to include what's
# necessary for a specific port in our own output.  (1) came first, but (2)
# is superior for many things, like when the same thing needs to be done for
# multiple ports and you don't want to duplicate files in multiple 
# subdirectories.  Much of the stuff done via Method 1 today should probably
# be converted to Method 2.  
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/Makefile,v 1.19 2003/05/15 16:35:29 momjian Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/port
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS+=dynloader.o pg_sema.o pg_shmem.o

OBJS+=$(TAS)

ifeq ($(PORTNAME), qnx4)
OBJS+=qnx4/SUBSYS.o
endif
ifeq ($(PORTNAME), beos)
OBJS+=beos/SUBSYS.o
endif
ifeq ($(PORTNAME), darwin)
OBJS+=darwin/SUBSYS.o
endif
ifeq ($(PORTNAME), win32)
OBJS+=win32/SUBSYS.o
endif

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) $@ $^

qnx4/SUBSYS.o: qnx4.dir

qnx4.dir:
	$(MAKE) -C qnx4 all

beos/SUBSYS.o: beos.dir

beos.dir:
	$(MAKE) -C beos all

darwin/SUBSYS.o: darwin.dir

darwin.dir:
	$(MAKE) -C darwin all

win32/SUBSYS.o: win32.dir

win32.dir:
	$(MAKE) -C win32 all

tas.o: tas.s
	$(CC) $(CFLAGS) -c $<

# IPC test program
ipc_test: ipc_test.o pg_sema.o pg_shmem.o
	$(CC) $(CFLAGS) $(LDFLAGS) $(export_dynamic) $^ $(LIBS) -o $@

distclean clean:
	rm -f SUBSYS.o $(OBJS) ipc_test ipc_test.o
	$(MAKE) -C beos clean
	$(MAKE) -C darwin clean
	$(MAKE) -C qnx4 clean
	$(MAKE) -C win32 clean

