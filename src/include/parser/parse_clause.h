/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *	  handle clauses in parser
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_clause.h,v 1.46 2006/07/03 22:45:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_CLAUSE_H
#define PARSE_CLAUSE_H

#include "parser/parse_node.h"

extern void transformFromClause(ParseState *pstate, List *frmList);
extern int setTargetTable(ParseState *pstate, RangeVar *relation,
			   bool inh, bool alsoSource, AclMode requiredPerms);
extern bool interpretInhOption(InhOption inhOpt);
extern bool interpretOidsOption(List *defList);

extern Node *transformWhereClause(ParseState *pstate, Node *clause,
					 const char *constructName);
extern Node *transformLimitClause(ParseState *pstate, Node *clause,
					 const char *constructName);
extern List *transformGroupClause(ParseState *pstate, List *grouplist,
					 List **targetlist, List *sortClause);
extern List *transformSortClause(ParseState *pstate, List *orderlist,
					List **targetlist, bool resolveUnknown);
extern List *transformDistinctClause(ParseState *pstate, List *distinctlist,
						List **targetlist, List **sortClause);

extern List *addAllTargetsToSortList(ParseState *pstate,
						List *sortlist, List *targetlist,
						bool resolveUnknown);
extern List *addTargetToSortList(ParseState *pstate, TargetEntry *tle,
					List *sortlist, List *targetlist,
					int sortby_kind, List *sortby_opname,
					bool resolveUnknown);
extern Index assignSortGroupRef(TargetEntry *tle, List *tlist);
extern bool targetIsInSortList(TargetEntry *tle, List *sortList);

#endif   /* PARSE_CLAUSE_H */
