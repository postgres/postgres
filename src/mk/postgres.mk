#-------------------------------------------------------------------------
#
# postgres.mk--
#    The master postgres makefile for implicit rules, definitions and 
#    variables. Every postgres makefile (except those that include 
#    postgres.subdir.mk only) should include this file.
#
# Copyright (c) 1994-5, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/mk/Attic/postgres.mk,v 1.3 1996/09/23 08:24:11 scrappy Exp $
#
#-------------------------------------------------------------------------


##############################################################################
#
# Default first rule (all):
#    This is here so that people doing "gmake" without arguments will
#    build the program (PROG), shell script (SHPROG) or library (LIB). To 
#    override this, you could define a rule before including postgres.mk.
#    (See .dosomething: for an explanation of its presence.)
#

ifdef PROG
all:	localobj $(PROG) .dosomething
else
ifdef SHPROG
all:	localobj $(SHPROG) .dosomething
else
ifdef LIB
#all:	localobj lib$(LIB).a install-headers .dosomething
all:	localobj lib$(LIB).a
else
# if you don't define PROG, SHPROG or LIB before including this, use :: for
# your all. (this is here so that clean is not the first rule)
all::	localobj
endif
endif
endif

##############################################################################
#
# Flags for programs (ar, yacc, etc.)
#

YFLAGS=		-d
RANLIB=		touch
AROPT=		crs
#AROPT=		cq
LINTFLAGS = 


#
# Installation. 
#
# This is the default for all platforms. If your platform uses a different
# BSD-style install program, change it in src/mk/port/postgres.mk.$PORTNAME
INSTALL=	installbsd

INSTLOPTS=	-c -m 444
INSTL_EXE_OPTS=	-c -m 555
INSTL_LIB_OPTS= -c -m 664

##############################################################################
#
# Canned command sequences
#

# making partial objects (if BIGOBJS is defined)
define make_partial
	$(LD) -r -o $(objdir)/$(@F) $(addprefix $(objdir)/,$(notdir $^))
endef

# compiling a .c which is generated (and is in $objdir)
define cc_inobjdir
	$(CC) -c $(CFLAGS) $(CPPFLAGS) $(objdir)/$(<F) -o $(objdir)/$(@F)
endef


##############################################################################
#
# Variables
#

# Makefile.global is where the user configurations are. (objdir is defined
# there)
include $(MKDIR)/../Makefile.global
-include $(MKDIR)/port/postgres.mk.$(PORTNAME)

CURDIR:= $(shell pwd)

# This is where we put all the .o's and the generated files.
VPATH:= $(CURDIR)/$(objdir)


##############################################################################
#
# General rules
#

.PHONY: clean .dosomething localobj beforeinstall

# clean up the objects and generated files
clean:
	@if test -d $(objdir); then cd $(objdir); rm -f $(CLEANFILES) ;else true; fi;

# just a matter of personal taste; make sure we do something and don't
# get this message: "gmake[1]: Nothing to be done for 'all'."
.dosomething: 
	@cat /dev/null

localobj:
	@if test ! -d $(objdir); then mkdir $(objdir); else true; fi;

#
# create the directories before doing install
#
ifndef NO_BEFOREINSTL
beforeinstall: localobj
	@-if test ! -d $(DESTDIR)$(LIBDIR); \
		then mkdir $(DESTDIR)$(LIBDIR); fi
	@-if test ! -d $(DESTDIR)$(BINDIR); \
		then mkdir $(DESTDIR)$(BINDIR); fi
	@-if test ! -d $(HEADERDIR); \
		then mkdir $(HEADERDIR); fi
else
beforeinstall: localobj
endif

##############################################################################
#
# Implicit rules
#

# building .o from C++ sources
$(objdir)/%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

# building .o from .c (in $objdir):
$(objdir)/%.o: %.c 
	$(CC) -c $(CFLAGS) $(CPPFLAGS) $< -o $(objdir)/$(@F)

# building .o from .s (in $objdir):
$(objdir)/%.o: %.s
	$(AS) $(ASFLAGS) $< -o $(objdir)/$(@F)

