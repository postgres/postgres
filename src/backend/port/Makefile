#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for the port-specific subsystem of the backend
#
# You must invoke this make file with the PORTNAME variable set to the
# name of a port, i.e. the name of a subdirectory of the current directory.
# Example: 
#
#    make SUBSYS.o PORTNAME=linux
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
#    $Header: /cvsroot/pgsql/src/backend/port/Makefile,v 1.9 1997/01/23 15:58:34 scrappy Exp $
#
#-------------------------------------------------------------------------

ifndef PORTNAME
.DEFAULT all: 
	@echo "Error:  Must invoke make with PORTNAME= argument."
	@false
else

OBJS = $(PORTNAME)/SUBSYS.o

ifneq (,$(findstring /$(PORTNAME)/, /i386_solaris/sparc_solaris/ultrix4/nextstep/svr4))
# Other ports get the inet_aton() function from their standard C libraries.
OBJS += inet_aton.o
endif

all: submake SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

.PHONY: submake clean dep

submake:
	$(MAKE) -C $(PORTNAME) SUBSYS.o

clean:
	rm -f $(OBJS)
	$(MAKE) -C $(PORTNAME) clean

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend
	$(MAKE) -C $(PORTNAME) $@

ifeq (depend,$(wildcard depend))
include depend
endif

endif

