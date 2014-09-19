#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for backend/commands
#
# IDENTIFICATION
#    src/backend/commands/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/commands
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = aggregatecmds.o alter.o analyze.o async.o cluster.o comment.o  \
	collationcmds.o constraint.o conversioncmds.o copy.o createas.o \
	dbcommands.o define.o discard.o dropcmds.o \
	event_trigger.o explain.o extension.o foreigncmds.o functioncmds.o \
	indexcmds.o lockcmds.o matview.o operatorcmds.o opclasscmds.o \
	policy.o portalcmds.o prepare.o proclang.o \
	schemacmds.o seclabel.o sequence.o tablecmds.o tablespace.o trigger.o \
	tsearchcmds.o typecmds.o user.o vacuum.o vacuumlazy.o \
	variable.o view.o

include $(top_srcdir)/src/backend/common.mk
