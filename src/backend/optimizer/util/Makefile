#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/util
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/optimizer/util/Makefile,v 1.17 2007/01/20 17:16:11 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/util
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = clauses.o joininfo.o pathnode.o plancat.o predtest.o \
       relnode.o restrictinfo.o tlist.o var.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS)
