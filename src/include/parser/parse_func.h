/*-------------------------------------------------------------------------
 *
 * catalog_utils.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_func.h,v 1.1 1997/11/25 22:06:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_FUNC_H
#define PARSER_FUNC_H

#include <nodes/nodes.h>
#include <nodes/pg_list.h>
#include <nodes/parsenodes.h>
#include <nodes/primnodes.h>
#include <parser/parse_func.h>
#include <parser/parse_node.h>

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

Node *ParseFunc(ParseState *pstate, char *funcname, List *fargs,
	int *curr_resno);

Oid funcid_get_rettype(Oid funcid);

CandidateList func_get_candidates(char *funcname, int nargs);

bool can_coerce(int nargs, Oid *input_typeids, Oid *func_typeids);

int match_argtypes(int nargs,
				   Oid *input_typeids,
				   CandidateList function_typeids,
				   CandidateList *candidates);

Oid * func_select_candidate(int nargs,
						  Oid *input_typeids,
						  CandidateList candidates);

bool func_get_detail(char *funcname,
					int nargs,
					Oid *oid_array,
					Oid *funcid,	/* return value */
					Oid *rettype,	/* return value */
					bool *retset,	/* return value */
					Oid **true_typeids);

Oid ** argtype_inherit(int nargs, Oid *oid_array);

int findsupers(Oid relid, Oid **supervec);

Oid **genxprod(InhPaths *arginh, int nargs);

void make_arguments(int nargs,
				   List *fargs,
				   Oid *input_typeids,
				   Oid *function_typeids);

List *setup_tlist(char *attname, Oid relid);

List *setup_base_tlist(Oid typeid);

Node *ParseComplexProjection(ParseState *pstate,
						   char *funcname,
						   Node *first_arg,
						   bool *attisset);
	
void func_error(char *caller, char *funcname, int nargs, Oid *argtypes);

				   


#endif							/* PARSE_FUNC_H */

