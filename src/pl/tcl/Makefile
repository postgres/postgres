#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for the pltcl shared object
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/pl/tcl/Makefile,v 1.2 1998/04/05 22:02:56 momjian Exp $
#
#-------------------------------------------------------------------------

#
# Tell make where the postgresql sources live
#
SRCDIR= ../../../src
include $(SRCDIR)/Makefile.global


#
# Include definitions from the tclConfig.sh file
#
include Makefile.tcldefs


#
# Uncomment the following to force a specific version of the
# Tcl shared library to be used.
#
#TCL_LIB_SPEC=-L/usr/lib -ltcl8.0


#
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

#
# Build the shared lib
#
all: $(INFILES)

Makefile.tcldefs:
	./mkMakefile.tcldefs

#
# Clean 
#
clean:
	rm -f $(INFILES)
	rm -f Makefile.tcldefs

install: all
	$(INSTALL) $(INSTL_LIB_OPTS) $(DLOBJS) $(LIBDIR)/$(DLOBJS)

