/*-------------------------------------------------------------------------
 *
 * parse_func.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_func.h,v 1.28 2001/01/24 19:43:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_FUNC_H
#define PARSER_FUNC_H

#include "parser/parse_node.h"

/*
 *	This structure is used to explore the inheritance hierarchy above
 *	nodes in the type tree in order to disambiguate among polymorphic
 *	functions.
 */
typedef struct _InhPaths
{
	int			nsupers;		/* number of superclasses */
	Oid			self;			/* this class */
	Oid		   *supervec;		/* vector of superclasses */
} InhPaths;

/*
 *	This structure holds a list of possible functions or operators that
 *	agree with the known name and argument types of the function/operator.
 */
typedef struct _CandidateList
{
	Oid		   *args;
	struct _CandidateList *next;
}		   *CandidateList;

extern Node *ParseNestedFuncOrColumn(ParseState *pstate, Attr *attr,
									 int precedence);
extern Node *ParseFuncOrColumn(ParseState *pstate,
							   char *funcname, List *fargs,
							   bool agg_star, bool agg_distinct,
							   int precedence);

extern bool func_get_detail(char *funcname, int nargs, Oid *argtypes,
							Oid *funcid, Oid *rettype,
							bool *retset, Oid **true_typeids);

extern bool typeInheritsFrom(Oid subclassTypeId, Oid superclassTypeId);

extern void func_error(char *caller, char *funcname,
		   int nargs, Oid *argtypes, char *msg);

#endif	 /* PARSE_FUNC_H */
