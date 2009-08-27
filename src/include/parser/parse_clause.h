/*-------------------------------------------------------------------------
 *
 * parse_clause.h
 *	  handle clauses in parser
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_clause.h,v 1.55.2.1 2009/08/27 20:08:12 tgl Exp $
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
					 List **targetlist, List *sortClause,
					 bool isWindowFunc);
extern List *transformSortClause(ParseState *pstate, List *orderlist,
					List **targetlist, bool resolveUnknown, bool isWindowFunc);

extern List *transformWindowDefinitions(ParseState *pstate,
						   List *windowdefs,
						   List **targetlist);

extern List *transformDistinctClause(ParseState *pstate,
						List **targetlist, List *sortClause);
extern List *transformDistinctOnClause(ParseState *pstate, List *distinctlist,
						  List **targetlist, List *sortClause);

extern Index assignSortGroupRef(TargetEntry *tle, List *tlist);
extern bool targetIsInSortList(TargetEntry *tle, Oid sortop, List *sortList);

#endif   /* PARSE_CLAUSE_H */
