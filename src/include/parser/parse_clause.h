/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_clause.h,v 1.20 2000/11/08 22:10:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_CLAUSE_H
#define PARSE_CLAUSE_H

#include "parser/parse_node.h"

extern void makeRangeTable(ParseState *pstate, List *frmList);
extern void lockTargetTable(ParseState *pstate, char *relname);
extern void setTargetTable(ParseState *pstate, char *relname,
						   bool inh, bool inJoinSet);
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
