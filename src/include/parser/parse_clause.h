/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_clause.h,v 1.4 1997/12/29 01:13:28 momjian Exp $
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

extern void makeRangeTable(ParseState *pstate, char *relname, List *frmList);
extern Node *transformWhereClause(ParseState *pstate, Node *a_expr);
extern List *transformGroupClause(ParseState *pstate, List *grouplist,
			List *targetlist);
extern List *transformSortClause(ParseState *pstate,
					List *orderlist, List *targetlist,
					char *uniqueFlag);
extern List *transformUnionClause(List *unionClause, List *targetlist);

#endif							/* PARSE_CLAUSE_H */

