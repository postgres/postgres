/*-------------------------------------------------------------------------
 *
 * partitionfuncs.c
 *	  Functions for accessing partition-related metadata
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/partitionfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/partition.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "funcapi.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * Checks if a given relation can be part of a partition tree.  Returns
 * false if the relation cannot be processed, in which case it is up to
 * the caller to decide what to do, by either raising an error or doing
 * something else.
 */
static bool
check_rel_can_be_partition(Oid relid)
{
	char		relkind;
	bool		relispartition;

	/* Check if relation exists */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		return false;

	relkind = get_rel_relkind(relid);
	relispartition = get_rel_relispartition(relid);

	/* Only allow relation types that can appear in partition trees. */
	if (!relispartition && !RELKIND_HAS_PARTITIONS(relkind))
		return false;

	return true;
}

/*
 * pg_partition_tree
 *
 * Produce a view with one row per member of a partition tree, beginning
 * from the top-most parent given by the caller.  This gives information
 * about each partition, its immediate partitioned parent, if it is
 * a leaf partition and its level in the hierarchy.
 */
Datum
pg_partition_tree(PG_FUNCTION_ARGS)
{
#define PG_PARTITION_TREE_COLS	4
	Oid			rootrelid = PG_GETARG_OID(0);
	FuncCallContext *funcctx;
	List	   *partitions;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;
		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		if (!check_rel_can_be_partition(rootrelid))
			SRF_RETURN_DONE(funcctx);

		/* switch to memory context appropriate for multiple function calls */
		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * Find all members of inheritance set.  We only need AccessShareLock
		 * on the children for the partition information lookup.
		 */
		partitions = find_all_inheritors(rootrelid, AccessShareLock, NULL);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = tupdesc;

		/* The only state we need is the partition list */
		funcctx->user_fctx = (void *) partitions;

		MemoryContextSwitchTo(oldcxt);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	partitions = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < list_length(partitions))
	{
		Datum		result;
		Datum		values[PG_PARTITION_TREE_COLS] = {0};
		bool		nulls[PG_PARTITION_TREE_COLS] = {0};
		HeapTuple	tuple;
		Oid			parentid = InvalidOid;
		Oid			relid = list_nth_oid(partitions, funcctx->call_cntr);
		char		relkind = get_rel_relkind(relid);
		int			level = 0;
		List	   *ancestors = get_partition_ancestors(relid);
		ListCell   *lc;

		/*
		 * Form tuple with appropriate data.
		 */

		/* relid */
		values[0] = ObjectIdGetDatum(relid);

		/* parentid */
		if (ancestors != NIL)
			parentid = linitial_oid(ancestors);
		if (OidIsValid(parentid))
			values[1] = ObjectIdGetDatum(parentid);
		else
			nulls[1] = true;

		/* isleaf */
		values[2] = BoolGetDatum(!RELKIND_HAS_PARTITIONS(relkind));

		/* level */
		if (relid != rootrelid)
		{
			foreach(lc, ancestors)
			{
				level++;
				if (lfirst_oid(lc) == rootrelid)
					break;
			}
		}
		values[3] = Int32GetDatum(level);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	/* done when there are no more elements left */
	SRF_RETURN_DONE(funcctx);
}

/*
 * pg_partition_root
 *
 * Returns the top-most parent of the partition tree to which a given
 * relation belongs, or NULL if it's not (or cannot be) part of any
 * partition tree.
 */
Datum
pg_partition_root(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Oid			rootrelid;
	List	   *ancestors;

	if (!check_rel_can_be_partition(relid))
		PG_RETURN_NULL();

	/* fetch the list of ancestors */
	ancestors = get_partition_ancestors(relid);

	/*
	 * If the input relation is already the top-most parent, just return
	 * itself.
	 */
	if (ancestors == NIL)
		PG_RETURN_OID(relid);

	rootrelid = llast_oid(ancestors);
	list_free(ancestors);

	/*
	 * "rootrelid" must contain a valid OID, given that the input relation is
	 * a valid partition tree member as checked above.
	 */
	Assert(OidIsValid(rootrelid));
	PG_RETURN_OID(rootrelid);
}

/*
 * pg_partition_ancestors
 *
 * Produces a view with one row per ancestor of the given partition,
 * including the input relation itself.
 */
Datum
pg_partition_ancestors(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	FuncCallContext *funcctx;
	List	   *ancestors;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;

		funcctx = SRF_FIRSTCALL_INIT();

		if (!check_rel_can_be_partition(relid))
			SRF_RETURN_DONE(funcctx);

		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ancestors = get_partition_ancestors(relid);
		ancestors = lcons_oid(relid, ancestors);

		/* The only state we need is the ancestors list */
		funcctx->user_fctx = (void *) ancestors;

		MemoryContextSwitchTo(oldcxt);
	}

	funcctx = SRF_PERCALL_SETUP();
	ancestors = (List *) funcctx->user_fctx;

	if (funcctx->call_cntr < list_length(ancestors))
	{
		Oid			resultrel = list_nth_oid(ancestors, funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, ObjectIdGetDatum(resultrel));
	}

	SRF_RETURN_DONE(funcctx);
}
