/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_clause.h,v 1.8 1999/02/23 08:05:27 thomas Exp $
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

extern void makeRangeTable(ParseState *pstate, char *relname, List *frmList, Node **qual);
extern Node *transformWhereClause(ParseState *pstate, Node *where, Node *using);
extern List *transformGroupClause(ParseState *pstate, List *grouplist,
					 List *targetlist);
extern List *transformSortClause(ParseState *pstate,
					List *orderlist, List *sortClause,
					List *targetlist, char *uniqueFlag);
extern List *transformUnionClause(List *unionClause, List *targetlist);

#endif	 /* PARSE_CLAUSE_H */
