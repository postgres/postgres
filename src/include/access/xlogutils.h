/*
 * xlogutils.h
 *
 * PostgreSQL transaction log manager utility routines
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: xlogutils.h,v 1.7 2001/03/22 04:00:32 momjian Exp $
 */
#ifndef XLOG_UTILS_H
#define XLOG_UTILS_H

#include "access/rmgr.h"
#include "utils/rel.h"

extern int XLogIsOwnerOfTuple(RelFileNode hnode, ItemPointer iptr,
				   TransactionId xid, CommandId cid);
extern bool XLogIsValidTuple(RelFileNode hnode, ItemPointer iptr);

extern void XLogOpenLogRelation(void);

extern void XLogInitRelationCache(void);
extern void XLogCloseRelationCache(void);

extern Relation XLogOpenRelation(bool redo, RmgrId rmid, RelFileNode rnode);
extern Buffer XLogReadBuffer(bool extend, Relation reln, BlockNumber blkno);

#endif
