#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for executor
#
# IDENTIFICATION
#    src/backend/executor/Makefile
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
       nodeHashjoin.o nodeIndexscan.o nodeIndexonlyscan.o \
       nodeLimit.o nodeLockRows.o \
       nodeMaterial.o nodeMergeAppend.o nodeMergejoin.o nodeModifyTable.o \
       nodeNestloop.o nodeFunctionscan.o nodeRecursiveunion.o nodeResult.o \
       nodeSeqscan.o nodeSetOp.o nodeSort.o nodeUnique.o \
       nodeValuesscan.o nodeCtescan.o nodeWorktablescan.o \
       nodeGroup.o nodeSubplan.o nodeSubqueryscan.o nodeTidscan.o \
       nodeForeignscan.o nodeWindowAgg.o tstoreReceiver.o spi.o

include $(top_srcdir)/src/backend/common.mk
