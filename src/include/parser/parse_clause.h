/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_clause.h,v 1.2 1997/11/26 01:14:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_CLAUSE_H
#define PARSE_CLAUSE_H

#include <nodes/pg_list.h>
#include <nodes/nodes.h>
#include <nodes/parsenodes.h>
#include <nodes/primnodes.h>
#include <parser/parse_node.h>

extern void parseFromClause(ParseState *pstate, List *frmList);
extern void makeRangeTable(ParseState *pstate, char *relname, List *frmList);
extern Node *transformWhereClause(ParseState *pstate, Node *a_expr);
extern TargetEntry *find_targetlist_entry(ParseState *pstate,
			SortGroupBy *sortgroupby, List *tlist);
extern List *transformGroupClause(ParseState *pstate, List *grouplist,
			List *targetlist);
extern List *transformSortClause(ParseState *pstate,
					List *orderlist, List *targetlist,
					char *uniqueFlag);

#endif							/* PARSE_CLAUSE_H */

