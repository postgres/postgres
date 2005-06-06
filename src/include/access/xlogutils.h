/*
 * xlogutils.h
 *
 * PostgreSQL transaction log manager utility routines
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/xlogutils.h,v 1.18 2005/06/06 17:01:25 tgl Exp $
 */
#ifndef XLOG_UTILS_H
#define XLOG_UTILS_H

#include "storage/buf.h"
#include "utils/rel.h"


extern void XLogInitRelationCache(void);
extern void XLogCloseRelationCache(void);

extern Relation XLogOpenRelation(RelFileNode rnode);
extern void XLogCloseRelation(RelFileNode rnode);

extern Buffer XLogReadBuffer(bool extend, Relation reln, BlockNumber blkno);

#endif
