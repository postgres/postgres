#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for commands
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/commands/Makefile,v 1.9 1997/12/17 04:44:47 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

INCLUDE_OPT = -I.. 

ifdef PORTNAME
INCLUDE_OPT+=-I../port/$(PORTNAME) 
endif

CFLAGS+=$(INCLUDE_OPT)

OBJS = async.o creatinh.o command.o copy.o defind.o define.o \
       remove.o rename.o vacuum.o version.o view.o cluster.o \
       recipe.o explain.o sequence.o trigger.o user.o proclang.o dbcommands.o

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

