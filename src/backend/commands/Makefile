#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/commands
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/commands/Makefile,v 1.39 2008/12/19 16:25:17 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/commands
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = aggregatecmds.o alter.o analyze.o async.o cluster.o comment.o  \
	conversioncmds.o copy.o \
	dbcommands.o define.o discard.o explain.o foreigncmds.o functioncmds.o \
	indexcmds.o lockcmds.o operatorcmds.o opclasscmds.o \
	portalcmds.o prepare.o proclang.o \
	schemacmds.o sequence.o tablecmds.o tablespace.o trigger.o \
	tsearchcmds.o typecmds.o user.o vacuum.o vacuumlazy.o \
	variable.o view.o

include $(top_srcdir)/src/backend/common.mk
