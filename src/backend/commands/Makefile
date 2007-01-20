#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/commands
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/commands/Makefile,v 1.35 2007/01/20 17:16:11 petere Exp $
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
	schemacmds.o sequence.o tablecmds.o tablespace.o trigger.o \
	typecmds.o user.o vacuum.o vacuumlazy.o variable.o view.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

clean: 
	rm -f SUBSYS.o $(OBJS) 
