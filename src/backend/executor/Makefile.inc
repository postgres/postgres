#-------------------------------------------------------------------------
#
# Makefile.inc--
#    Makefile for the executor module
#
# Copyright (c) 1994, Regents of the University of California
#
#
# IDENTIFICATION
#    $Header: /cvsroot/pgsql/src/backend/executor/Attic/Makefile.inc,v 1.1.1.1 1996/07/09 06:21:24 scrappy Exp $
#
#-------------------------------------------------------------------------

VPATH:= $(VPATH):$(CURDIR)/executor

SRCS_EXECUTOR= execAmi.c execFlatten.c execJunk.c execMain.c \
	execProcnode.c execQual.c execScan.c execTuples.c \
	execUtils.c functions.c nodeAppend.c nodeAgg.c nodeHash.c \
	nodeHashjoin.c nodeIndexscan.c nodeMaterial.c nodeMergejoin.c \
	nodeNestloop.c nodeResult.c nodeSeqscan.c nodeSort.c \
	nodeUnique.c nodeTee.c nodeGroup.c

HEADERS+= execFlatten.h execdebug.h execdefs.h execdesc.h \
	executor.h functions.h hashjoin.h nodeAgg.h nodeAppend.h \
	nodeHash.h nodeHashjoin.h nodeIndexscan.h nodeMaterial.h \
	nodeMergejoin.h nodeNestloop.h nodeResult.h  \
	nodeSeqscan.h nodeSort.h nodeUnique.h tuptable.h nodeTee.h \
	nodeGroup.h

