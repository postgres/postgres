#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for executor
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/executor/Makefile,v 1.12 2000/05/29 05:44:45 tgl Exp $
#
#-------------------------------------------------------------------------

SRCDIR = ../..
include ../../Makefile.global

OBJS = execAmi.o execFlatten.o execJunk.o execMain.o \
       execProcnode.o execQual.o execScan.o execTuples.o \
       execUtils.o functions.o nodeAppend.o nodeAgg.o nodeHash.o \
       nodeHashjoin.o nodeIndexscan.o nodeMaterial.o nodeMergejoin.o \
       nodeNestloop.o nodeResult.o nodeSeqscan.o nodeSort.o \
       nodeUnique.o nodeGroup.o spi.o nodeSubplan.o \
	nodeTidscan.o

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

