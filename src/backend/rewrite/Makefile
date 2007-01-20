#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for rewrite
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/rewrite/Makefile,v 1.16 2007/01/20 17:16:12 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/rewrite
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = rewriteRemove.o rewriteDefine.o \
       rewriteHandler.o rewriteManip.o rewriteSupport.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS) 
