#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for executor
#
# IDENTIFICATION
#    $PostgreSQL: pgsql/src/backend/executor/Makefile,v 1.27 2008/02/19 10:30:07 petere Exp $
#
#-------------------------------------------------------------------------

subdir = src/backend/executor
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = execAmi.o execCurrent.o execGrouping.o execJunk.o execMain.o \
       execProcnode.o execQual.o execScan.o execTuples.o \
       execUtils.o functions.o instrument.o nodeAppend.o nodeAgg.o \
       nodeBitmapAnd.o nodeBitmapOr.o \
       nodeBitmapHeapscan.o nodeBitmapIndexscan.o nodeHash.o \
       nodeHashjoin.o nodeIndexscan.o nodeMaterial.o nodeMergejoin.o \
       nodeNestloop.o nodeFunctionscan.o nodeResult.o nodeSeqscan.o \
       nodeSetOp.o nodeSort.o nodeUnique.o \
       nodeValuesscan.o nodeLimit.o nodeGroup.o \
       nodeSubplan.o nodeSubqueryscan.o nodeTidscan.o tstoreReceiver.o spi.o

include $(top_srcdir)/src/backend/common.mk
