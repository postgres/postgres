/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_clause.h,v 1.24 2001/03/22 04:00:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_CLAUSE_H
#define PARSE_CLAUSE_H

#include "parser/parse_node.h"

extern void transformFromClause(ParseState *pstate, List *frmList);
extern int setTargetTable(ParseState *pstate, char *relname,
			   bool inh, bool alsoSource);
extern bool interpretInhOption(InhOption inhOpt);
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
