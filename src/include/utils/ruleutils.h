/*-------------------------------------------------------------------------
 *
 * ruleutils.h
 *		Declarations for ruleutils.c
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/ruleutils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RULEUTILS_H
#define RULEUTILS_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"


extern char *pg_get_indexdef_string(Oid indexrelid);
extern char *pg_get_indexdef_columns(Oid indexrelid, bool pretty);

extern char *pg_get_constraintdef_string(Oid constraintId);
extern char *deparse_expression(Node *expr, List *dpcontext,
				   bool forceprefix, bool showimplicit);
extern List *deparse_context_for(const char *aliasname, Oid relid);
extern List *deparse_context_for_planstate(Node *planstate, List *ancestors,
							  List *rtable, List *rtable_names);
extern List *select_rtable_names_for_explain(List *rtable,
								Bitmapset *rels_used);
extern char *generate_collation_name(Oid collid);

#endif	/* RULEUTILS_H */
