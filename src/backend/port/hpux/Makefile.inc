#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/hpux (HP-UX specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/hpux/Attic/Makefile.inc,v 1.2 1996/10/18 00:33:22 scrappy Exp $
#
#-------------------------------------------------------------------------

#
# HP-UX needs:
# -W l,-E	export symbols for linking with the shared libraries 
#		dynamic loader
# -W p,-H400000	expand cpp #define table size so the Nodes files don't 
#		break it
#
# -W p,-H400000
ifeq ($(CC), cc)
CFLAGS+= -W l,-E 
LDFLAGS+= -W l,-E
LDADD+= -ll -ldld
else
ifeq ($(CC), gcc)
LDADD+= -ll /usr/lib/libdld.sl
endif
endif

#
# The #pragma trick required on 8.07 no longer works -- the #pragma
# is thoroughly broken.  However, the +u flag has been extended to
# handle alignment requirement arguments (defaulting to 2) for things 
# other than struct references, so the #pragma is no longer needed.
#

#
# (1) The YACC grammar is too big..
# (HP-UX 9.0x, x<2, added basic block limits for +O2; 9.0x, x>=2, changed
# the syntax to something else.)
#
# (2) The 9.00 optimizer chokes on some of our source.
#
#.if (${HPUX_MAJOR} == "09")
#.  if !defined(CDEBUG)
#.    if (${HPUX_MINOR} == "00" || ${HPUX_MINOR} == "01")
#CFLAGS+= +Obb600
#CFLAGS+= -DWEAK_C_OPTIMIZER
#.    else
#CFLAGS+= +Onolimit
#.    endif
#.  endif
#.endif

HEADERS+= fixade.h machine.h port-protos.h

SUBSRCS+= dynloader.c port.c tas.s
