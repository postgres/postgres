/*-------------------------------------------------------------------------
 *
 * rewriteHandler.h
 *		External interface to query rewriter.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/rewrite/rewriteHandler.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEHANDLER_H
#define REWRITEHANDLER_H

#include "nodes/parsenodes.h"
#include "utils/relcache.h"

extern List *QueryRewrite(Query *parsetree);
extern void AcquireRewriteLocks(Query *parsetree,
								bool forExecute,
								bool forUpdatePushedDown);

extern Node *build_column_default(Relation rel, int attrno);

extern Query *get_view_query(Relation view);
extern bool view_has_instead_trigger(Relation view, CmdType event,
									 List *mergeActionList);
extern const char *view_query_is_auto_updatable(Query *viewquery,
												bool check_cols);
extern int	relation_is_updatable(Oid reloid,
								  List *outer_reloids,
								  bool include_triggers,
								  Bitmapset *include_cols);
extern void error_view_not_updatable(Relation view,
									 CmdType command,
									 List *mergeActionList,
									 const char *detail);

#endif							/* REWRITEHANDLER_H */
