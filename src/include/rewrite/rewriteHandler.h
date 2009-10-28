/*-------------------------------------------------------------------------
 *
 * rewriteHandler.h
 *		External interface to query rewriter.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/rewrite/rewriteHandler.h,v 1.32 2009/10/28 17:36:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEHANDLER_H
#define REWRITEHANDLER_H

#include "utils/relcache.h"
#include "nodes/parsenodes.h"

extern List *QueryRewrite(Query *parsetree);
extern void AcquireRewriteLocks(Query *parsetree, bool forUpdatePushedDown);
extern Node *build_column_default(Relation rel, int attrno);

#endif   /* REWRITEHANDLER_H */
