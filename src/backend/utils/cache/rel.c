/*-------------------------------------------------------------------------
 *
 * rel.c
 *	  POSTGRES relation descriptor code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/rel.c,v 1.4 1999/02/13 23:19:43 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/* #define RELREFDEBUG	1 */

#include "postgres.h"
#include "miscadmin.h"
#include "access/istrat.h"
#include "access/tupdesc.h"
#include "utils/rel.h"
#include "storage/fd.h"


/*
 *		RelationIsValid is now a macro in rel.h -cim 4/27/91
 *
 *		Many of the RelationGet...() functions are now macros in rel.h
 *				-mer 3/2/92
 */

/*
 * RelationGetIndexStrategy 
 *		Returns index strategy for a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 *		Assumes relation descriptor is for an index relation.
 */
IndexStrategy
RelationGetIndexStrategy(Relation relation)
{
	return relation->rd_istrat;
}

/*
 * RelationSetIndexSupport 
 *		Sets index strategy and support info for a relation.
 *
 * Note:
 *		Assumes relation descriptor is a valid pointer to sufficient space.
 *		Assumes index strategy is valid.  Assumes support is valid if non-
 *		NULL.
 */
/* ----------------
 *		RelationSetIndexSupport
 *
 *		This routine saves two pointers -- one to the IndexStrategy, and
 *		one to the RegProcs that support the indexed access method.  These
 *		pointers are stored in the space following the attribute data in the
 *		reldesc.
 *
 *	 NEW:  the index strategy and support are now stored in real fields
 *		   at the end of the structure					  - jolly
 * ----------------
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
