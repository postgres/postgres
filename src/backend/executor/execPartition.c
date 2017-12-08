/*-------------------------------------------------------------------------
 *
 * execPartition.c
 *	  Support routines for partitioning.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execPartition.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_inherits_fn.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"

static PartitionDispatch *RelationGetPartitionDispatchInfo(Relation rel,
								 int *num_parted, List **leaf_part_oids);
static void get_partition_dispatch_recurse(Relation rel, Relation parent,
							   List **pds, List **leaf_part_oids);
static void FormPartitionKeyDatum(PartitionDispatch pd,
					  TupleTableSlot *slot,
					  EState *estate,
					  Datum *values,
					  bool *isnull);
static char *ExecBuildSlotPartitionKeyDescription(Relation rel,
									 Datum *values,
									 bool *isnull,
									 int maxfieldlen);

/*
 * ExecSetupPartitionTupleRouting - set up information needed during
 * tuple routing for partitioned tables
 *
 * Output arguments:
 * 'pd' receives an array of PartitionDispatch objects with one entry for
 *		every partitioned table in the partition tree
 * 'partitions' receives an array of ResultRelInfo* objects with one entry for
 *		every leaf partition in the partition tree
 * 'tup_conv_maps' receives an array of TupleConversionMap objects with one
 *		entry for every leaf partition (required to convert input tuple based
 *		on the root table's rowtype to a leaf partition's rowtype after tuple
 *		routing is done)
 * 'partition_tuple_slot' receives a standalone TupleTableSlot to be used
 *		to manipulate any given leaf partition's rowtype after that partition
 *		is chosen by tuple-routing.
 * 'num_parted' receives the number of partitioned tables in the partition
 *		tree (= the number of entries in the 'pd' output array)
 * 'num_partitions' receives the number of leaf partitions in the partition
 *		tree (= the number of entries in the 'partitions' and 'tup_conv_maps'
 *		output arrays
 *
 * Note that all the relations in the partition tree are locked using the
 * RowExclusiveLock mode upon return from this function.
 */
void
ExecSetupPartitionTupleRouting(ModifyTableState *mtstate,
							   Relation rel,
							   Index resultRTindex,
							   EState *estate,
							   PartitionDispatch **pd,
							   ResultRelInfo ***partitions,
							   TupleConversionMap ***tup_conv_maps,
							   TupleTableSlot **partition_tuple_slot,
							   int *num_parted, int *num_partitions)
{
	TupleDesc	tupDesc = RelationGetDescr(rel);
	List	   *leaf_parts;
	ListCell   *cell;
	int			i;
	ResultRelInfo *leaf_part_rri;

	/*
	 * Get the information about the partition tree after locking all the
	 * partitions.
	 */
	(void) find_all_inheritors(RelationGetRelid(rel), RowExclusiveLock, NULL);
	*pd = RelationGetPartitionDispatchInfo(rel, num_parted, &leaf_parts);
	*num_partitions = list_length(leaf_parts);
	*partitions = (ResultRelInfo **) palloc(*num_partitions *
											sizeof(ResultRelInfo *));
	*tup_conv_maps = (TupleConversionMap **) palloc0(*num_partitions *
													 sizeof(TupleConversionMap *));

	/*
	 * Initialize an empty slot that will be used to manipulate tuples of any
	 * given partition's rowtype.  It is attached to the caller-specified node
	 * (such as ModifyTableState) and released when the node finishes
	 * processing.
	 */
	*partition_tuple_slot = MakeTupleTableSlot();

	leaf_part_rri = (ResultRelInfo *) palloc0(*num_partitions *
											  sizeof(ResultRelInfo));
	i = 0;
	foreach(cell, leaf_parts)
	{
		Relation	partrel;
		TupleDesc	part_tupdesc;

		/*
		 * We locked all the partitions above including the leaf partitions.
		 * Note that each of the relations in *partitions are eventually
		 * closed by the caller.
		 */
		partrel = heap_open(lfirst_oid(cell), NoLock);
		part_tupdesc = RelationGetDescr(partrel);

		/*
		 * Save a tuple conversion map to convert a tuple routed to this
		 * partition from the parent's type to the partition's.
		 */
		(*tup_conv_maps)[i] = convert_tuples_by_name(tupDesc, part_tupdesc,
													 gettext_noop("could not convert row type"));

		InitResultRelInfo(leaf_part_rri,
						  partrel,
						  resultRTindex,
						  rel,
						  estate->es_instrument);

		/*
		 * Verify result relation is a valid target for INSERT.
		 */
		CheckValidResultRel(leaf_part_rri, CMD_INSERT);

		/*
		 * Open partition indices.  The user may have asked to check for
		 * conflicts within this leaf partition and do "nothing" instead of
		 * throwing an error.  Be prepared in that case by initializing the
		 * index information needed by ExecInsert() to perform speculative
		 * insertions.
		 */
		if (leaf_part_rri->ri_RelationDesc->rd_rel->relhasindex &&
			leaf_part_rri->ri_IndexRelationDescs == NULL)
			ExecOpenIndices(leaf_part_rri,
							mtstate != NULL &&
							mtstate->mt_onconflict != ONCONFLICT_NONE);

		estate->es_leaf_result_relations =
			lappend(estate->es_leaf_result_relations, leaf_part_rri);

		(*partitions)[i] = leaf_part_rri++;
		i++;
	}
}

/*
 * ExecFindPartition -- Find a leaf partition in the partition tree rooted
 * at parent, for the heap tuple contained in *slot
 *
 * estate must be non-NULL; we'll need it to compute any expressions in the
 * partition key(s)
 *
 * If no leaf partition is found, this routine errors out with the appropriate
 * error message, else it returns the leaf partition sequence number
 * as an index into the array of (ResultRelInfos of) all leaf partitions in
 * the partition tree.
 */
int
ExecFindPartition(ResultRelInfo *resultRelInfo, PartitionDispatch *pd,
				  TupleTableSlot *slot, EState *estate)
{
	int			result;
	Datum		values[PARTITION_MAX_KEYS];
	bool		isnull[PARTITION_MAX_KEYS];
	Relation	rel;
	PartitionDispatch parent;
	ExprContext *ecxt = GetPerTupleExprContext(estate);
	TupleTableSlot *ecxt_scantuple_old = ecxt->ecxt_scantuple;

	/*
	 * First check the root table's partition constraint, if any.  No point in
	 * routing the tuple if it doesn't belong in the root table itself.
	 */
	if (resultRelInfo->ri_PartitionCheck)
		ExecPartitionCheck(resultRelInfo, slot, estate);

	/* start with the root partitioned table */
	parent = pd[0];
	while (true)
	{
		PartitionDesc partdesc;
		TupleTableSlot *myslot = parent->tupslot;
		TupleConversionMap *map = parent->tupmap;
		int			cur_index = -1;

		rel = parent->reldesc;
		partdesc = RelationGetPartitionDesc(rel);

		/*
		 * Convert the tuple to this parent's layout so that we can do certain
		 * things we do below.
		 */
		if (myslot != NULL && map != NULL)
		{
			HeapTuple	tuple = ExecFetchSlotTuple(slot);

			ExecClearTuple(myslot);
			tuple = do_convert_tuple(tuple, map);
			ExecStoreTuple(tuple, myslot, InvalidBuffer, true);
			slot = myslot;
		}

		/*
		 * Extract partition key from tuple. Expression evaluation machinery
		 * that FormPartitionKeyDatum() invokes expects ecxt_scantuple to
		 * point to the correct tuple slot.  The slot might have changed from
		 * what was used for the parent table if the table of the current
		 * partitioning level has different tuple descriptor from the parent.
		 * So update ecxt_scantuple accordingly.
		 */
		ecxt->ecxt_scantuple = slot;
		FormPartitionKeyDatum(parent, slot, estate, values, isnull);

		/*
		 * Nothing for get_partition_for_tuple() to do if there are no
		 * partitions to begin with.
		 */
		if (partdesc->nparts == 0)
		{
			result = -1;
			break;
		}

		cur_index = get_partition_for_tuple(rel, values, isnull);

		/*
		 * cur_index < 0 means we failed to find a partition of this parent.
		 * cur_index >= 0 means we either found the leaf partition, or the
		 * next parent to find a partition of.
		 */
		if (cur_index < 0)
		{
			result = -1;
			break;
		}
		else if (parent->indexes[cur_index] >= 0)
		{
			result = parent->indexes[cur_index];
			break;
		}
		else
			parent = pd[-parent->indexes[cur_index]];
	}

	/* A partition was not found. */
	if (result < 0)
	{
		char	   *val_desc;

		val_desc = ExecBuildSlotPartitionKeyDescription(rel,
														values, isnull, 64);
		Assert(OidIsValid(RelationGetRelid(rel)));
		ereport(ERROR,
				(errcode(ERRCODE_CHECK_VIOLATION),
				 errmsg("no partition of relation \"%s\" found for row",
						RelationGetRelationName(rel)),
				 val_desc ? errdetail("Partition key of the failing row contains %s.", val_desc) : 0));
	}

	ecxt->ecxt_scantuple = ecxt_scantuple_old;
	return result;
}

/*
 * RelationGetPartitionDispatchInfo
 *		Returns information necessary to route tuples down a partition tree
 *
 * The number of elements in the returned array (that is, the number of
 * PartitionDispatch objects for the partitioned tables in the partition tree)
 * is returned in *num_parted and a list of the OIDs of all the leaf
 * partitions of rel is returned in *leaf_part_oids.
 *
 * All the relations in the partition tree (including 'rel') must have been
 * locked (using at least the AccessShareLock) by the caller.
 */
static PartitionDispatch *
RelationGetPartitionDispatchInfo(Relation rel,
								 int *num_parted, List **leaf_part_oids)
{
	List	   *pdlist = NIL;
	PartitionDispatchData **pd;
	ListCell   *lc;
	int			i;

	Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	*num_parted = 0;
	*leaf_part_oids = NIL;

	get_partition_dispatch_recurse(rel, NULL, &pdlist, leaf_part_oids);
	*num_parted = list_length(pdlist);
	pd = (PartitionDispatchData **) palloc(*num_parted *
										   sizeof(PartitionDispatchData *));
	i = 0;
	foreach(lc, pdlist)
	{
		pd[i++] = lfirst(lc);
	}

	return pd;
}

/*
 * get_partition_dispatch_recurse
 *		Recursively expand partition tree rooted at rel
 *
 * As the partition tree is expanded in a depth-first manner, we maintain two
 * global lists: of PartitionDispatch objects corresponding to partitioned
 * tables in *pds and of the leaf partition OIDs in *leaf_part_oids.
 *
 * Note that the order of OIDs of leaf partitions in leaf_part_oids matches
 * the order in which the planner's expand_partitioned_rtentry() processes
 * them.  It's not necessarily the case that the offsets match up exactly,
 * because constraint exclusion might prune away some partitions on the
 * planner side, whereas we'll always have the complete list; but unpruned
 * partitions will appear in the same order in the plan as they are returned
 * here.
 */
static void
get_partition_dispatch_recurse(Relation rel, Relation parent,
							   List **pds, List **leaf_part_oids)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	PartitionDesc partdesc = RelationGetPartitionDesc(rel);
	PartitionKey partkey = RelationGetPartitionKey(rel);
	PartitionDispatch pd;
	int			i;

	check_stack_depth();

	/* Build a PartitionDispatch for this table and add it to *pds. */
	pd = (PartitionDispatch) palloc(sizeof(PartitionDispatchData));
	*pds = lappend(*pds, pd);
	pd->reldesc = rel;
	pd->key = partkey;
	pd->keystate = NIL;
	pd->partdesc = partdesc;
	if (parent != NULL)
	{
		/*
		 * For every partitioned table other than the root, we must store a
		 * tuple table slot initialized with its tuple descriptor and a tuple
		 * conversion map to convert a tuple from its parent's rowtype to its
		 * own. That is to make sure that we are looking at the correct row
		 * using the correct tuple descriptor when computing its partition key
		 * for tuple routing.
		 */
		pd->tupslot = MakeSingleTupleTableSlot(tupdesc);
		pd->tupmap = convert_tuples_by_name(RelationGetDescr(parent),
											tupdesc,
											gettext_noop("could not convert row type"));
	}
	else
	{
		/* Not required for the root partitioned table */
		pd->tupslot = NULL;
		pd->tupmap = NULL;
	}

	/*
	 * Go look at each partition of this table.  If it's a leaf partition,
	 * simply add its OID to *leaf_part_oids.  If it's a partitioned table,
	 * recursively call get_partition_dispatch_recurse(), so that its
	 * partitions are processed as well and a corresponding PartitionDispatch
	 * object gets added to *pds.
	 *
	 * About the values in pd->indexes: for a leaf partition, it contains the
	 * leaf partition's position in the global list *leaf_part_oids minus 1,
	 * whereas for a partitioned table partition, it contains the partition's
	 * position in the global list *pds multiplied by -1.  The latter is
	 * multiplied by -1 to distinguish partitioned tables from leaf partitions
	 * when going through the values in pd->indexes.  So, for example, when
	 * using it during tuple-routing, encountering a value >= 0 means we found
	 * a leaf partition.  It is immediately returned as the index in the array
	 * of ResultRelInfos of all the leaf partitions, using which we insert the
	 * tuple into that leaf partition.  A negative value means we found a
	 * partitioned table.  The value multiplied by -1 is returned as the index
	 * in the array of PartitionDispatch objects of all partitioned tables in
	 * the tree.  This value is used to continue the search in the next level
	 * of the partition tree.
	 */
	pd->indexes = (int *) palloc(partdesc->nparts * sizeof(int));
	for (i = 0; i < partdesc->nparts; i++)
	{
		Oid			partrelid = partdesc->oids[i];

		if (get_rel_relkind(partrelid) != RELKIND_PARTITIONED_TABLE)
		{
			*leaf_part_oids = lappend_oid(*leaf_part_oids, partrelid);
			pd->indexes[i] = list_length(*leaf_part_oids) - 1;
		}
		else
		{
			/*
			 * We assume all tables in the partition tree were already locked
			 * by the caller.
			 */
			Relation	partrel = heap_open(partrelid, NoLock);

			pd->indexes[i] = -list_length(*pds);
			get_partition_dispatch_recurse(partrel, rel, pds, leaf_part_oids);
		}
	}
}

/* ----------------
 *		FormPartitionKeyDatum
 *			Construct values[] and isnull[] arrays for the partition key
 *			of a tuple.
 *
 *	pd				Partition dispatch object of the partitioned table
 *	slot			Heap tuple from which to extract partition key
 *	estate			executor state for evaluating any partition key
 *					expressions (must be non-NULL)
 *	values			Array of partition key Datums (output area)
 *	isnull			Array of is-null indicators (output area)
 *
 * the ecxt_scantuple slot of estate's per-tuple expr context must point to
 * the heap tuple passed in.
 * ----------------
 */
static void
FormPartitionKeyDatum(PartitionDispatch pd,
					  TupleTableSlot *slot,
					  EState *estate,
					  Datum *values,
					  bool *isnull)
{
	ListCell   *partexpr_item;
	int			i;

	if (pd->key->partexprs != NIL && pd->keystate == NIL)
	{
		/* Check caller has set up context correctly */
		Assert(estate != NULL &&
			   GetPerTupleExprContext(estate)->ecxt_scantuple == slot);

		/* First time through, set up expression evaluation state */
		pd->keystate = ExecPrepareExprList(pd->key->partexprs, estate);
	}

	partexpr_item = list_head(pd->keystate);
	for (i = 0; i < pd->key->partnatts; i++)
	{
		AttrNumber	keycol = pd->key->partattrs[i];
		Datum		datum;
		bool		isNull;

		if (keycol != 0)
		{
			/* Plain column; get the value directly from the heap tuple */
			datum = slot_getattr(slot, keycol, &isNull);
		}
		else
		{
			/* Expression; need to evaluate it */
			if (partexpr_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");
			datum = ExecEvalExprSwitchContext((ExprState *) lfirst(partexpr_item),
											  GetPerTupleExprContext(estate),
											  &isNull);
			partexpr_item = lnext(partexpr_item);
		}
		values[i] = datum;
		isnull[i] = isNull;
	}

	if (partexpr_item != NULL)
		elog(ERROR, "wrong number of partition key expressions");
}

/*
 * ExecBuildSlotPartitionKeyDescription
 *
 * This works very much like BuildIndexValueDescription() and is currently
 * used for building error messages when ExecFindPartition() fails to find
 * partition for a row.
 */
static char *
ExecBuildSlotPartitionKeyDescription(Relation rel,
									 Datum *values,
									 bool *isnull,
									 int maxfieldlen)
{
	StringInfoData buf;
	PartitionKey key = RelationGetPartitionKey(rel);
	int			partnatts = get_partition_natts(key);
	int			i;
	Oid			relid = RelationGetRelid(rel);
	AclResult	aclresult;

	if (check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		return NULL;

	/* If the user has table-level access, just go build the description. */
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		/*
		 * Step through the columns of the partition key and make sure the
		 * user has SELECT rights on all of them.
		 */
		for (i = 0; i < partnatts; i++)
		{
			AttrNumber	attnum = get_partition_col_attnum(key, i);

			/*
			 * If this partition key column is an expression, we return no
			 * detail rather than try to figure out what column(s) the
			 * expression includes and if the user has SELECT rights on them.
			 */
			if (attnum == InvalidAttrNumber ||
				pg_attribute_aclcheck(relid, attnum, GetUserId(),
									  ACL_SELECT) != ACLCHECK_OK)
				return NULL;
		}
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "(%s) = (",
					 pg_get_partkeydef_columns(relid, true));

	for (i = 0; i < partnatts; i++)
	{
		char	   *val;
		int			vallen;

		if (isnull[i])
			val = "null";
		else
		{
			Oid			foutoid;
			bool		typisvarlena;

			getTypeOutputInfo(get_partition_col_typid(key, i),
							  &foutoid, &typisvarlena);
			val = OidOutputFunctionCall(foutoid, values[i]);
		}

		if (i > 0)
			appendStringInfoString(&buf, ", ");

		/* truncate if needed */
		vallen = strlen(val);
		if (vallen <= maxfieldlen)
			appendStringInfoString(&buf, val);
		else
		{
			vallen = pg_mbcliplen(val, vallen, maxfieldlen);
			appendBinaryStringInfo(&buf, val, vallen);
			appendStringInfoString(&buf, "...");
		}
	}

	appendStringInfoChar(&buf, ')');

	return buf.data;
}
