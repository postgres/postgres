/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_clause.h,v 1.14 2000/01/26 05:58:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_CLAUSE_H
#define PARSE_CLAUSE_H

#include "parser/parse_node.h"

extern void makeRangeTable(ParseState *pstate, List *frmList, Node **qual);
extern void setTargetTable(ParseState *pstate, char *relname);
extern Node *transformWhereClause(ParseState *pstate, Node *where,
								  Node *using);
extern List *transformGroupClause(ParseState *pstate, List *grouplist,
								  List *targetlist);
extern List *transformSortClause(ParseState *pstate, List *orderlist,
								 List *targetlist, char *uniqueFlag);

extern List *addAllTargetsToSortList(List *sortlist, List *targetlist);
extern Index assignSortGroupRef(TargetEntry *tle, List *tlist);

#endif	 /* PARSE_CLAUSE_H */
