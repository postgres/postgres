#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for commands
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/commands/Makefile,v 1.24 2000/05/29 05:44:44 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include $(SRCDIR)/Makefile.global

OBJS = async.o creatinh.o command.o comment.o copy.o indexcmds.o define.o \
       remove.o rename.o vacuum.o view.o cluster.o \
       explain.o sequence.o trigger.o user.o proclang.o \
       dbcommands.o variable.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS) 

ifeq (depend,$(wildcard depend))
include depend
endif

