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

OBJS = execAmi.o execCurrent.o execExpr.o execExprInterp.o \
       execGrouping.o execIndexing.o execJunk.o \
       execMain.o execMerge.o execParallel.o execPartition.o execProcnode.o \
       execReplication.o execScan.o execSRF.o execTuples.o \
       execUtils.o functions.o instrument.o nodeAppend.o nodeAgg.o \
       nodeBitmapAnd.o nodeBitmapOr.o \
       nodeBitmapHeapscan.o nodeBitmapIndexscan.o \
       nodeCustom.o nodeFunctionscan.o nodeGather.o \
       nodeHash.o nodeHashjoin.o nodeIndexscan.o nodeIndexonlyscan.o \
       nodeLimit.o nodeLockRows.o nodeGatherMerge.o \
       nodeMaterial.o nodeMergeAppend.o nodeMergejoin.o nodeModifyTable.o \
       nodeNestloop.o nodeProjectSet.o nodeRecursiveunion.o nodeResult.o \
       nodeSamplescan.o nodeSeqscan.o nodeSetOp.o nodeSort.o nodeUnique.o \
       nodeValuesscan.o \
       nodeCtescan.o nodeNamedtuplestorescan.o nodeWorktablescan.o \
       nodeGroup.o nodeSubplan.o nodeSubqueryscan.o nodeTidscan.o \
       nodeForeignscan.o nodeWindowAgg.o tstoreReceiver.o tqueue.o spi.o \
       nodeTableFuncscan.o

include $(top_srcdir)/src/backend/common.mk
