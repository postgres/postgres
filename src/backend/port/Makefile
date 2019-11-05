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
#    src/backend/port/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/port
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(TAS) \
	atomics.o \
	pg_sema.o \
	pg_shmem.o

ifeq ($(PORTNAME), win32)
SUBDIRS += win32
endif

include $(top_srcdir)/src/backend/common.mk

tas.o: tas.s
ifeq ($(SUN_STUDIO_CC), yes)
# preprocess assembler file with cpp
	$(CC) $(CFLAGS) -c -P $<
	mv $*.i $*_cpp.s
	$(CC) $(CFLAGS) -c $*_cpp.s -o $@
else
	$(CC) $(CFLAGS) -c $<
endif

distclean clean:
	rm -f tas_cpp.s
	$(MAKE) -C win32 clean
