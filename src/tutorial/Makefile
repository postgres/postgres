#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for tutorial/C-code
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/tutorial/Makefile,v 1.1.1.1 1996/07/09 06:22:33 scrappy Exp $
#
#-------------------------------------------------------------------------

MKDIR=	../mk
include $(MKDIR)/postgres.mk

VPATH:= $(VPATH):C-code

#
# build dynamically-loaded object files
#
DLOBJS= complex$(SLSUFF) funcs$(SLSUFF) 

#
# ... plus test query inputs
#
CREATEFILES= $(DLOBJS:%=$(objdir)/%) \
	advanced.sql basics.sql complex.sql funcs.sql syscat.sql

include $(MKDIR)/postgres.user.mk

CFLAGS+= -I$(srcdir)/backend

CLEANFILES+= $(notdir $(CREATEFILES))

all:: $(CREATEFILES)



