#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for commands
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/commands/Makefile,v 1.15 1998/07/26 04:30:22 scrappy Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = async.o creatinh.o command.o copy.o defind.o define.o \
       remove.o rename.o vacuum.o version.o view.o cluster.o \
       recipe.o explain.o sequence.o trigger.o user.o proclang.o \
       dbcommands.o variable.o

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

