#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for executor
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/executor/Makefile,v 1.7 1998/04/06 00:22:26 momjian Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

CFLAGS += -I..

OBJS = execAmi.o execFlatten.o execJunk.o execMain.o \
       execProcnode.o execQual.o execScan.o execTuples.o \
       execUtils.o functions.o nodeAppend.o nodeAgg.o nodeHash.o \
       nodeHashjoin.o nodeIndexscan.o nodeMaterial.o nodeMergejoin.o \
       nodeNestloop.o nodeResult.o nodeSeqscan.o nodeSort.o \
       nodeUnique.o nodeTee.o nodeGroup.o spi.o nodeSubplan.o

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

