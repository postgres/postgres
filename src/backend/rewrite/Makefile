#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for rewrite
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/rewrite/Makefile,v 1.8 1998/07/26 04:30:37 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..
ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = rewriteRemove.o rewriteDefine.o \
       rewriteHandler.o rewriteManip.o rewriteSupport.o locks.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

