#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/commands
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/commands/Makefile,v 1.32 2003/06/27 14:45:27 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/commands
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = aggregatecmds.o alter.o analyze.o async.o cluster.o comment.o  \
	conversioncmds.o copy.o \
	dbcommands.o define.o explain.o functioncmds.o \
	indexcmds.o lockcmds.o operatorcmds.o opclasscmds.o \
	portalcmds.o prepare.o proclang.o \
	schemacmds.o sequence.o tablecmds.o trigger.o typecmds.o user.o \
	vacuum.o vacuumlazy.o variable.o view.o

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
