#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for port/aix (AIX specific stuff)
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/port/aix/Attic/Makefile.inc,v 1.2 1996/07/22 22:59:27 scrappy Exp $
#
#-------------------------------------------------------------------------

#
# aix has fast linkers and don't need the BIGOBJ stuff.
#
BIGOBJS=false

CFLAGS+= -DCLASS_CONFLICT -DDISABLE_XOPEN_NLS -DNEED_ISINF

LDFLAGS+= -bE:$(objdir)/$(PROG).exp

LDADD+= -ll -lld

HEADERS+= dlfcn.h machine.h port-protos.h

SUBSRCS+= dlfcn.c 

MKLDEXPORT=$(POSTGRESDIR)/src/backend/port/aix/mkldexport.sh

${PROG}.exp: ${PROG}.noexp
	mv -f $(objdir)/${PROG}.noexp $(objdir)/${PROG}
	$(MKLDEXPORT) $(objdir)/${PROG} ${BINDIR} > $(objdir)/${PROG}.exp
	mv -f $(objdir)/${PROG} $(objdir)/${PROG}.noexp

${PROG}.noexp: ${OBJS}
	touch -f $(objdir)/${PROG}.exp
	${CC} ${LDFLAGS} -o $(objdir)/${PROG}.noexp ${OBJS} ${LDADD}

EXPORTS= ${PROG}.exp

CLEANFILES+= ${PROG}.noexp ${PROG}.exp
