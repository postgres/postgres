#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for commands
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/commands/Makefile,v 1.20 1999/10/26 16:32:46 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..

ifdef MULTIBYTE
CFLAGS+= $(MBFLAGS)
endif

OBJS = async.o creatinh.o command.o comment.o copy.o indexcmds.o define.o \
       remove.o rename.o vacuum.o view.o cluster.o \
       explain.o sequence.o trigger.o user.o proclang.o \
       dbcommands.o variable.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) -r -o SUBSYS.o $(OBJS)

# The following declares a hard-coded dependency on parse.h since, 
# if compiled without make dep, comment.c would get compiled before 
# the parser. 

comment.o: ../parse.h

../parse.h:
	$(MAKE) -C .. parse.h

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

