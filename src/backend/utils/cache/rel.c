/*-------------------------------------------------------------------------
 *
 * rel.c
 *	  POSTGRES relation descriptor code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/rel.c,v 1.9 2001/01/24 19:43:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/istrat.h"


/*
 *		RelationIsValid is now a macro in rel.h -cim 4/27/91
 *
 *		All of the RelationGet...() functions are now macros in rel.h
 *				-mer 3/2/92
 */

/*
 * RelationSetIndexSupport
 *		Sets index strategy and support info for a relation.
 *
 *		This routine saves two pointers -- one to the IndexStrategy, and
 *		one to the RegProcs that support the indexed access method.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 *		Assumes index strategy is valid.  Assumes support is valid if non-
 *		NULL.
 */
void
RelationSetIndexSupport(Relation relation,
						IndexStrategy strategy,
						RegProcedure *support)
{
	Assert(PointerIsValid(relation));
	Assert(IndexStrategyIsValid(strategy));

	relation->rd_istrat = strategy;
	relation->rd_support = support;
}
