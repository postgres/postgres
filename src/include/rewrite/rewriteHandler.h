/*-------------------------------------------------------------------------
 *
 * rewriteHandler.h
 *		External interface to query rewriter.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/rewrite/rewriteHandler.h,v 1.31 2009/01/01 17:24:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEHANDLER_H
#define REWRITEHANDLER_H

#include "utils/relcache.h"
#include "nodes/parsenodes.h"

extern List *QueryRewrite(Query *parsetree);
extern void AcquireRewriteLocks(Query *parsetree);
extern Node *build_column_default(Relation rel, int attrno);

#endif   /* REWRITEHANDLER_H */
