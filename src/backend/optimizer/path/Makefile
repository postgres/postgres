#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/path
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/optimizer/path/Makefile,v 1.17 2007/01/20 17:16:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/path
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = allpaths.o clausesel.o costsize.o indxpath.o \
       joinpath.o joinrels.o orindxpath.o pathkeys.o tidpath.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
