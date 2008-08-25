/*-------------------------------------------------------------------------
 *
 * nodeFuncs.h
 *		Various general-purpose manipulations of Node trees
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/nodes/nodeFuncs.h,v 1.27 2008/08/25 22:42:34 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFUNCS_H
#define NODEFUNCS_H

#include "nodes/parsenodes.h"


/* flags bits for query_tree_walker and query_tree_mutator */
#define QTW_IGNORE_RT_SUBQUERIES	0x01		/* subqueries in rtable */
#define QTW_IGNORE_JOINALIASES		0x02		/* JOIN alias var lists */
#define QTW_DONT_COPY_QUERY			0x04		/* do not copy top Query */


extern Oid	exprType(Node *expr);
extern int32 exprTypmod(Node *expr);
extern bool exprIsLengthCoercion(Node *expr, int32 *coercedTypmod);
extern bool expression_returns_set(Node *clause);

extern bool expression_tree_walker(Node *node, bool (*walker) (),
											   void *context);
extern Node *expression_tree_mutator(Node *node, Node *(*mutator) (),
												 void *context);

extern bool query_tree_walker(Query *query, bool (*walker) (),
										  void *context, int flags);
extern Query *query_tree_mutator(Query *query, Node *(*mutator) (),
											 void *context, int flags);

extern bool range_table_walker(List *rtable, bool (*walker) (),
										   void *context, int flags);
extern List *range_table_mutator(List *rtable, Node *(*mutator) (),
											 void *context, int flags);

extern bool query_or_expression_tree_walker(Node *node, bool (*walker) (),
												   void *context, int flags);
extern Node *query_or_expression_tree_mutator(Node *node, Node *(*mutator) (),
												   void *context, int flags);

#endif   /* NODEFUNCS_H */
