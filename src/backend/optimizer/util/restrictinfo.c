/*-------------------------------------------------------------------------
 *
 * restrictinfo.c
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/restrictinfo.c,v 1.9 2000/01/26 05:56:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/internal.h"
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
