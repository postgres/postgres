/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_clause.h,v 1.16 2000/02/15 03:38:28 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_CLAUSE_H
#define PARSE_CLAUSE_H

#include "parser/parse_node.h"

extern void makeRangeTable(ParseState *pstate, List *frmList);
extern void setTargetTable(ParseState *pstate, char *relname);
extern Node *transformWhereClause(ParseState *pstate, Node *where);
extern List *transformGroupClause(ParseState *pstate, List *grouplist,
								  List *targetlist);
extern List *transformSortClause(ParseState *pstate, List *orderlist,
								 List *targetlist);
extern List *transformDistinctClause(ParseState *pstate, List *distinctlist,
									 List *targetlist, List **sortClause);

extern List *addAllTargetsToSortList(List *sortlist, List *targetlist);
extern Index assignSortGroupRef(TargetEntry *tle, List *tlist);

#endif	 /* PARSE_CLAUSE_H */
