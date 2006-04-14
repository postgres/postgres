/*
 * xlogutils.h
 *
 * PostgreSQL transaction log manager utility routines
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/xlogutils.h,v 1.21 2006/04/14 20:27:24 tgl Exp $
 */
#ifndef XLOG_UTILS_H
#define XLOG_UTILS_H

#include "storage/buf.h"
#include "utils/rel.h"


extern void XLogInitRelationCache(void);
extern void XLogCheckInvalidPages(void);
extern void XLogCloseRelationCache(void);

extern Relation XLogOpenRelation(RelFileNode rnode);
extern void XLogDropRelation(RelFileNode rnode);
extern void XLogDropDatabase(Oid dbid);
extern void XLogTruncateRelation(RelFileNode rnode, BlockNumber nblocks);

extern Buffer XLogReadBuffer(Relation reln, BlockNumber blkno, bool init);

#endif
