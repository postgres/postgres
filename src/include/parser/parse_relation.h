/*-------------------------------------------------------------------------
 *
 * parse_relation.h
 *	  prototypes for parse_relation.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_relation.h,v 1.19 2000/09/12 21:07:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_RELATION_H
#define PARSE_RELATION_H

#include "parser/parse_node.h"

extern Node *refnameRangeOrJoinEntry(ParseState *pstate,
									 char *refname,
									 int *sublevels_up);
extern RangeTblEntry *refnameRangeTableEntry(ParseState *pstate,
											 char *refname);
extern int refnameRangeTablePosn(ParseState *pstate,
								 char *refname,
								 int *sublevels_up);
extern int RTERangeTablePosn(ParseState *pstate,
							 RangeTblEntry *rte,
							 int *sublevels_up);
extern JoinExpr *scanJoinTreeForRefname(Node *jtnode, char *refname);
extern Node *colnameToVar(ParseState *pstate, char *colname);
extern Node *qualifiedNameToVar(ParseState *pstate, char *refname,
								char *colname, bool implicitRTEOK);
extern RangeTblEntry *addRangeTableEntry(ParseState *pstate,
										 char *relname,
										 Attr *alias,
										 bool inh,
										 bool inFromCl);
extern void addRTEtoJoinTree(ParseState *pstate, RangeTblEntry *rte);
extern RangeTblEntry *addImplicitRTE(ParseState *pstate, char *relname);
extern void expandRTE(ParseState *pstate, RangeTblEntry *rte,
					  List **colnames, List **colvars);
extern List *expandRelAttrs(ParseState *pstate, RangeTblEntry *rte);
extern List *expandJoinAttrs(ParseState *pstate, JoinExpr *join,
							 int sublevels_up);
extern int	attnameAttNum(Relation rd, char *a);
extern int	specialAttNum(char *a);
extern Oid	attnumTypeId(Relation rd, int attid);

#endif	 /* PARSE_RELATION_H */
