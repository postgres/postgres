#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the pltcl shared object
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/pl/tcl/Makefile,v 1.9 1998/10/18 19:41:00 tgl Exp $
#
#-------------------------------------------------------------------------

#
# Tell make where the postgresql sources live
#
SRCDIR= ../../../src
include $(SRCDIR)/Makefile.global

#
# Include definitions from the tclConfig.sh file
# NOTE: GNU make will make this file automatically if it doesn't exist,
# using the make rule that appears below.  Cute, eh?
#
include Makefile.tcldefs

#
# Find out whether Tcl was built as a shared library --- if not,
# we can't link a shared library that depends on it, and have to
# forget about building pltcl.
# In Tcl 8, tclConfig.sh sets TCL_SHARED_BUILD for us, but in
# older Tcl releases it doesn't.  In that case we guess based on
# the name of the Tcl library.
#
ifndef TCL_SHARED_BUILD
ifneq (,$(findstring $(DLSUFFIX),$(TCL_LIB_FILE)))
TCL_SHARED_BUILD=1
else
TCL_SHARED_BUILD=0
endif
endif


# Change following to how shared library that contain
# correct references to libtcl must get built on your system.
# Since these definitions come from the tclConfig.sh script,
# they should work if the shared build of tcl was successful
# on this system.
#
%$(TCL_SHLIB_SUFFIX):	%.o
	$(TCL_SHLIB_LD) -o $@ $< $(TCL_SHLIB_LD_LIBS) $(TCL_LIB_SPEC) $(TCL_LIBS)


#
# Uncomment the following to enable the unknown command lookup
# on the first of all calls to the call handler. See the doc
# in the modules directory about details.
#
#CFLAGS+= -DPLTCL_UNKNOWN_SUPPORT


CC = $(TCL_CC)
CFLAGS+= -I$(LIBPQDIR) -I$(SRCDIR)/include $(TCL_SHLIB_CFLAGS)

# For fmgr.h
CFLAGS+= -I$(SRCDIR)/backend

CFLAGS+= $(TCL_DEFS)

LDADD+= -L$(LIBPQDIR) -lpq
        
#
# DLOBJS is the dynamically-loaded object file.
#
DLOBJS= pltcl$(DLSUFFIX)

INFILES= $(DLOBJS) 

#
# plus exports files
#
ifdef EXPSUFF
INFILES+= $(DLOBJS:.o=$(EXPSUFF))
endif


ifeq ($(TCL_SHARED_BUILD),1)

#
# Build the shared lib
#
all: $(INFILES)

install: all
	$(INSTALL) $(INSTL_SHLIB_OPTS) $(DLOBJS) $(LIBDIR)/$(DLOBJS)

else

#
# Oops, can't build it
#
all:
	@echo "Cannot build pltcl because Tcl is not a shared library; skipping it."

install:
	@echo "Cannot build pltcl because Tcl is not a shared library; skipping it."

endif

#
# Make targets that are still valid when we can't build pltcl
# should be below here.
#

Makefile.tcldefs: mkMakefile.tcldefs.sh
	/bin/sh mkMakefile.tcldefs.sh

#
# Clean 
#
clean:
	rm -f $(INFILES) *.o
	rm -f Makefile.tcldefs

dep depend:
