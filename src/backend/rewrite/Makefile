#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for rewrite
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/rewrite/Makefile,v 1.1 1996/10/27 09:51:38 bryanh Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. \
              -I../port/$(PORTNAME) \
              -I../include \
              -I../../include

CFLAGS+=$(INCLUDE_OPT)

OBJS = rewriteRemove.o rewriteDefine.o \
       rewriteHandler.o rewriteManip.o rewriteSupport.o locks.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(INCLUDE_OPT) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

