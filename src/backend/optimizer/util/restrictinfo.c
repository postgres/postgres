/*-------------------------------------------------------------------------
 *
 * restrictinfo.c
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/restrictinfo.c,v 1.13 2001/01/24 19:43:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "optimizer/clauses.h"
#include "optimizer/restrictinfo.h"

/*
 * restriction_is_or_clause
 *
 * Returns t iff the restrictinfo node contains an 'or' clause.
 *
 */
bool
restriction_is_or_clause(RestrictInfo *restrictinfo)
{
	if (restrictinfo != NULL &&
		or_clause((Node *) restrictinfo->clause))
		return true;
	else
		return false;
}

/*
 * get_actual_clauses
 *
 * Returns a list containing the clauses from 'restrictinfo_list'.
 *
 */
List *
get_actual_clauses(List *restrictinfo_list)
{
	List	   *result = NIL;
	List	   *temp;

	foreach(temp, restrictinfo_list)
	{
		RestrictInfo *clause = (RestrictInfo *) lfirst(temp);

		result = lappend(result, clause->clause);
	}
	return result;
}

/*
 * get_actual_join_clauses
 *
 * Extract clauses from 'restrictinfo_list', separating those that
 * syntactically match the join level from those that were pushed down.
 */
void
get_actual_join_clauses(List *restrictinfo_list,
						List **joinquals, List **otherquals)
{
	List	   *temp;

	*joinquals = NIL;
	*otherquals = NIL;

	foreach(temp, restrictinfo_list)
	{
		RestrictInfo *clause = (RestrictInfo *) lfirst(temp);

		if (clause->ispusheddown)
			*otherquals = lappend(*otherquals, clause->clause);
		else
			*joinquals = lappend(*joinquals, clause->clause);
	}
}
