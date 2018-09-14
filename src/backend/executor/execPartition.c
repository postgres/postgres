/*-------------------------------------------------------------------------
 *
 * execPartition.c
 *	  Support routines for partitioning.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execPartition.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "partitioning/partbounds.h"
#include "partitioning/partprune.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"


/*-----------------------
 * PartitionDispatch - information about one partitioned table in a partition
 * hierarchy required to route a tuple to one of its partitions
 *
 *	reldesc		Relation descriptor of the table
 *	key			Partition key information of the table
 *	keystate	Execution state required for expressions in the partition key
 *	partdesc	Partition descriptor of the table
 *	tupslot		A standalone TupleTableSlot initialized with this table's tuple
 *				descriptor
 *	tupmap		TupleConversionMap to convert from the parent's rowtype to
 *				this table's rowtype (when extracting the partition key of a
 *				tuple just before routing it through this table)
 *	indexes		Array with partdesc->nparts members (for details on what
 *				individual members represent, see how they are set in
 *				get_partition_dispatch_recurse())
 *-----------------------
 */
typedef struct PartitionDispatchData
{
	Relation	reldesc;
	PartitionKey key;
	List	   *keystate;		/* list of ExprState */
	PartitionDesc partdesc;
	TupleTableSlot *tupslot;
	TupleConversionMap *tupmap;
	int		   *indexes;
} PartitionDispatchData;


static PartitionDispatch *RelationGetPartitionDispatchInfo(Relation rel,
								 int *num_parted, List **leaf_part_oids);
static void get_partition_dispatch_recurse(Relation rel, Relation parent,
							   List **pds, List **leaf_part_oids);
static void FormPartitionKeyDatum(PartitionDispatch pd,
					  TupleTableSlot *slot,
					  EState *estate,
					  Datum *values,
					  bool *isnull);
static int get_partition_for_tuple(Relation relation, Datum *values,
						bool *isnull);
static char *ExecBuildSlotPartitionKeyDescription(Relation rel,
									 Datum *values,
									 bool *isnull,
									 int maxfieldlen);
static List *adjust_partition_tlist(List *tlist, TupleConversionMap *map);
static void find_matching_subplans_recurse(PartitionPruningData *prunedata,
							   PartitionedRelPruningData *pprune,
							   bool initial_prune,
							   Bitmapset **validsubplans);


/*
 * ExecSetupPartitionTupleRouting - sets up information needed during
 * tuple routing for partitioned tables, encapsulates it in
 * PartitionTupleRouting, and returns it.
 *
 * Note that all the relations in the partition tree are locked using the
 * RowExclusiveLock mode upon return from this function.
 *
 * While we allocate the arrays of pointers of ResultRelInfo and
 * TupleConversionMap for all partitions here, actual objects themselves are
 * lazily allocated for a given partition if a tuple is actually routed to it;
 * see ExecInitPartitionInfo.  However, if the function is invoked for update
 * tuple routing, caller would already have initialized ResultRelInfo's for
 * some of the partitions, which are reused and assigned to their respective
 * slot in the aforementioned array.  For such partitions, we delay setting
 * up objects such as TupleConversionMap until those are actually chosen as
 * the partitions to route tuples to.  See ExecPrepareTupleRouting.
 */
PartitionTupleRouting *
ExecSetupPartitionTupleRouting(ModifyTableState *mtstate, Relation rel)
{
	List	   *leaf_parts;
	ListCell   *cell;
	int			i;
	ResultRelInfo *update_rri = NULL;
	int			num_update_rri = 0,
				update_rri_index = 0;
	PartitionTupleRouting *proute;
	int			nparts;
	ModifyTable *node = mtstate ? (ModifyTable *) mtstate->ps.plan : NULL;

	/*
	 * Get the information about the partition tree after locking all the
	 * partitions.
	 */
	(void) find_all_inheritors(RelationGetRelid(rel), RowExclusiveLock, NULL);
	proute = (PartitionTupleRouting *) palloc0(sizeof(PartitionTupleRouting));
	proute->partition_dispatch_info =
		RelationGetPartitionDispatchInfo(rel, &proute->num_dispatch,
										 &leaf_parts);
	proute->num_partitions = nparts = list_length(leaf_parts);
	proute->partitions =
		(ResultRelInfo **) palloc(nparts * sizeof(ResultRelInfo *));
	proute->parent_child_tupconv_maps =
		(TupleConversionMap **) palloc0(nparts * sizeof(TupleConversionMap *));
	proute->partition_oids = (Oid *) palloc(nparts * sizeof(Oid));

	/* Set up details specific to the type of tuple routing we are doing. */
	if (node && node->operation == CMD_UPDATE)
	{
		update_rri = mtstate->resultRelInfo;
		num_update_rri = list_length(node->plans);
		proute->subplan_partition_offsets =
			palloc(num_update_rri * sizeof(int));
		proute->num_subplan_partition_offsets = num_update_rri;

		/*
		 * We need an additional tuple slot for storing transient tuples that
		 * are converted to the root table descriptor.
		 */
		proute->root_tuple_slot = MakeTupleTableSlot(NULL);
	}

	/*
	 * Initialize an empty slot that will be used to manipulate tuples of any
	 * given partition's rowtype.  It is attached to the caller-specified node
	 * (such as ModifyTableState) and released when the node finishes
	 * processing.
	 */
	proute->partition_tuple_slot = MakeTupleTableSlot(NULL);

	i = 0;
	foreach(cell, leaf_parts)
	{
		ResultRelInfo *leaf_part_rri = NULL;
		Oid			leaf_oid = lfirst_oid(cell);

		proute->partition_oids[i] = leaf_oid;

		/*
		 * If the leaf partition is already present in the per-subplan result
		 * rels, we re-use that rather than initialize a new result rel. The
		 * per-subplan resultrels and the resultrels of the leaf partitions
		 * are both in the same canonical order. So while going through the
		 * leaf partition oids, we need to keep track of the next per-subplan
		 * result rel to be looked for in the leaf partition resultrels.
		 */
		if (update_rri_index < num_update_rri &&
			RelationGetRelid(update_rri[update_rri_index].ri_RelationDesc) == leaf_oid)
		{
			leaf_part_rri = &update_rri[update_rri_index];

			/*
			 * This is required in order to convert the partition's tuple to
			 * be compatible with the root partitioned table's tuple
			 * descriptor.  When generating the per-subplan result rels, this
			 * was not set.
			 */
			leaf_part_rri->ri_PartitionRoot = rel;

			/* Remember the subplan offset for this ResultRelInfo */
			proute->subplan_partition_offsets[update_rri_index] = i;

			update_rri_index++;
		}

		proute->partitions[i] = leaf_part_rri;
		i++;
	}

	/*
	 * For UPDATE, we should have found all the per-subplan resultrels in the
	 * leaf partitions.  (If this is an INSERT, both values will be zero.)
	 */
	Assert(update_rri_index == num_update_rri);

	return proute;
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
	PartitionDispatch dispatch;
	ExprContext *ecxt = GetPerTupleExprContext(estate);
	TupleTableSlot *ecxt_scantuple_old = ecxt->ecxt_scantuple;
	TupleTableSlot *myslot = NULL;
	MemoryContext	oldcxt;
	HeapTuple		tuple;

	/* use per-tuple context here to avoid leaking memory */
	oldcxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	/*
	 * First check the root table's partition constraint, if any.  No point in
	 * routing the tuple if it doesn't belong in the root table itself.
	 */
	if (resultRelInfo->ri_PartitionCheck)
		ExecPartitionCheck(resultRelInfo, slot, estate, true);

	/* start with the root partitioned table */
	tuple = ExecFetchSlotTuple(slot);
	dispatch = pd[0];
	while (true)
	{
		PartitionDesc partdesc;
		TupleConversionMap *map = dispatch->tupmap;
		int			cur_index = -1;

		rel = dispatch->reldesc;
		partdesc = RelationGetPartitionDesc(rel);

		/*
		 * Convert the tuple to this parent's layout, if different from the
		 * current relation.
		 */
		myslot = dispatch->tupslot;
		if (myslot != NULL && map != NULL)
		{
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
		FormPartitionKeyDatum(dispatch, slot, estate, values, isnull);

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
		else if (dispatch->indexes[cur_index] >= 0)
		{
			result = dispatch->indexes[cur_index];
			/* success! */
			break;
		}
		else
		{
			/* move down one level */
			dispatch = pd[-dispatch->indexes[cur_index]];

			/*
			 * Release the dedicated slot, if it was used.  Create a copy of
			 * the tuple first, for the next iteration.
			 */
			if (slot == myslot)
			{
				tuple = ExecCopySlotTuple(myslot);
				ExecClearTuple(myslot);
			}
		}
	}

	/* Release the tuple in the lowest parent's dedicated slot. */
	if (slot == myslot)
		ExecClearTuple(myslot);

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

	MemoryContextSwitchTo(oldcxt);
	ecxt->ecxt_scantuple = ecxt_scantuple_old;

	return result;
}

/*
 * ExecInitPartitionInfo
 *		Initialize ResultRelInfo and other information for a partition
 *
 * Returns the ResultRelInfo
 */
ResultRelInfo *
ExecInitPartitionInfo(ModifyTableState *mtstate,
					  ResultRelInfo *resultRelInfo,
					  PartitionTupleRouting *proute,
					  EState *estate, int partidx)
{
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	Relation	rootrel = resultRelInfo->ri_RelationDesc,
				partrel;
	Relation	firstResultRel = mtstate->resultRelInfo[0].ri_RelationDesc;
	ResultRelInfo *leaf_part_rri;
	MemoryContext oldContext;
	AttrNumber *part_attnos = NULL;
	bool		found_whole_row;

	/*
	 * We locked all the partitions in ExecSetupPartitionTupleRouting
	 * including the leaf partitions.
	 */
	partrel = heap_open(proute->partition_oids[partidx], NoLock);

	/*
	 * Keep ResultRelInfo and other information for this partition in the
	 * per-query memory context so they'll survive throughout the query.
	 */
	oldContext = MemoryContextSwitchTo(estate->es_query_cxt);

	leaf_part_rri = makeNode(ResultRelInfo);
	InitResultRelInfo(leaf_part_rri,
					  partrel,
					  node ? node->nominalRelation : 1,
					  rootrel,
					  estate->es_instrument);

	/*
	 * Verify result relation is a valid target for an INSERT.  An UPDATE of a
	 * partition-key becomes a DELETE+INSERT operation, so this check is still
	 * required when the operation is CMD_UPDATE.
	 */
	CheckValidResultRel(leaf_part_rri, CMD_INSERT);

	/*
	 * Since we've just initialized this ResultRelInfo, it's not in any list
	 * attached to the estate as yet.  Add it, so that it can be found later.
	 *
	 * Note that the entries in this list appear in no predetermined order,
	 * because partition result rels are initialized as and when they're
	 * needed.
	 */
	estate->es_tuple_routing_result_relations =
		lappend(estate->es_tuple_routing_result_relations,
				leaf_part_rri);

	/*
	 * Open partition indices.  The user may have asked to check for conflicts
	 * within this leaf partition and do "nothing" instead of throwing an
	 * error.  Be prepared in that case by initializing the index information
	 * needed by ExecInsert() to perform speculative insertions.
	 */
	if (partrel->rd_rel->relhasindex &&
		leaf_part_rri->ri_IndexRelationDescs == NULL)
		ExecOpenIndices(leaf_part_rri,
						(node != NULL &&
						 node->onConflictAction != ONCONFLICT_NONE));

	/*
	 * Build WITH CHECK OPTION constraints for the partition.  Note that we
	 * didn't build the withCheckOptionList for partitions within the planner,
	 * but simple translation of varattnos will suffice.  This only occurs for
	 * the INSERT case or in the case of UPDATE tuple routing where we didn't
	 * find a result rel to reuse in ExecSetupPartitionTupleRouting().
	 */
	if (node && node->withCheckOptionLists != NIL)
	{
		List	   *wcoList;
		List	   *wcoExprs = NIL;
		ListCell   *ll;
		int			firstVarno = mtstate->resultRelInfo[0].ri_RangeTableIndex;

		/*
		 * In the case of INSERT on a partitioned table, there is only one
		 * plan.  Likewise, there is only one WCO list, not one per partition.
		 * For UPDATE, there are as many WCO lists as there are plans.
		 */
		Assert((node->operation == CMD_INSERT &&
				list_length(node->withCheckOptionLists) == 1 &&
				list_length(node->plans) == 1) ||
			   (node->operation == CMD_UPDATE &&
				list_length(node->withCheckOptionLists) ==
				list_length(node->plans)));

		/*
		 * Use the WCO list of the first plan as a reference to calculate
		 * attno's for the WCO list of this partition.  In the INSERT case,
		 * that refers to the root partitioned table, whereas in the UPDATE
		 * tuple routing case, that refers to the first partition in the
		 * mtstate->resultRelInfo array.  In any case, both that relation and
		 * this partition should have the same columns, so we should be able
		 * to map attributes successfully.
		 */
		wcoList = linitial(node->withCheckOptionLists);

		/*
		 * Convert Vars in it to contain this partition's attribute numbers.
		 */
		part_attnos =
			convert_tuples_by_name_map(RelationGetDescr(partrel),
									   RelationGetDescr(firstResultRel),
									   gettext_noop("could not convert row type"));
		wcoList = (List *)
			map_variable_attnos((Node *) wcoList,
								firstVarno, 0,
								part_attnos,
								RelationGetDescr(firstResultRel)->natts,
								RelationGetForm(partrel)->reltype,
								&found_whole_row);
		/* We ignore the value of found_whole_row. */

		foreach(ll, wcoList)
		{
			WithCheckOption *wco = castNode(WithCheckOption, lfirst(ll));
			ExprState  *wcoExpr = ExecInitQual(castNode(List, wco->qual),
											   &mtstate->ps);

			wcoExprs = lappend(wcoExprs, wcoExpr);
		}

		leaf_part_rri->ri_WithCheckOptions = wcoList;
		leaf_part_rri->ri_WithCheckOptionExprs = wcoExprs;
	}

	/*
	 * Build the RETURNING projection for the partition.  Note that we didn't
	 * build the returningList for partitions within the planner, but simple
	 * translation of varattnos will suffice.  This only occurs for the INSERT
	 * case or in the case of UPDATE tuple routing where we didn't find a
	 * result rel to reuse in ExecSetupPartitionTupleRouting().
	 */
	if (node && node->returningLists != NIL)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;
		List	   *returningList;
		int			firstVarno = mtstate->resultRelInfo[0].ri_RangeTableIndex;

		/* See the comment above for WCO lists. */
		Assert((node->operation == CMD_INSERT &&
				list_length(node->returningLists) == 1 &&
				list_length(node->plans) == 1) ||
			   (node->operation == CMD_UPDATE &&
				list_length(node->returningLists) ==
				list_length(node->plans)));

		/*
		 * Use the RETURNING list of the first plan as a reference to
		 * calculate attno's for the RETURNING list of this partition.  See
		 * the comment above for WCO lists for more details on why this is
		 * okay.
		 */
		returningList = linitial(node->returningLists);

		/*
		 * Convert Vars in it to contain this partition's attribute numbers.
		 */
		if (part_attnos == NULL)
			part_attnos =
				convert_tuples_by_name_map(RelationGetDescr(partrel),
										   RelationGetDescr(firstResultRel),
										   gettext_noop("could not convert row type"));
		returningList = (List *)
			map_variable_attnos((Node *) returningList,
								firstVarno, 0,
								part_attnos,
								RelationGetDescr(firstResultRel)->natts,
								RelationGetForm(partrel)->reltype,
								&found_whole_row);
		/* We ignore the value of found_whole_row. */

		leaf_part_rri->ri_returningList = returningList;

		/*
		 * Initialize the projection itself.
		 *
		 * Use the slot and the expression context that would have been set up
		 * in ExecInitModifyTable() for projection's output.
		 */
		Assert(mtstate->ps.ps_ResultTupleSlot != NULL);
		slot = mtstate->ps.ps_ResultTupleSlot;
		Assert(mtstate->ps.ps_ExprContext != NULL);
		econtext = mtstate->ps.ps_ExprContext;
		leaf_part_rri->ri_projectReturning =
			ExecBuildProjectionInfo(returningList, econtext, slot,
									&mtstate->ps, RelationGetDescr(partrel));
	}

	/* Set up information needed for routing tuples to the partition. */
	ExecInitRoutingInfo(mtstate, estate, proute, leaf_part_rri, partidx);

	/*
	 * If there is an ON CONFLICT clause, initialize state for it.
	 */
	if (node && node->onConflictAction != ONCONFLICT_NONE)
	{
		TupleConversionMap *map = proute->parent_child_tupconv_maps[partidx];
		int			firstVarno = mtstate->resultRelInfo[0].ri_RangeTableIndex;
		TupleDesc	partrelDesc = RelationGetDescr(partrel);
		ExprContext *econtext = mtstate->ps.ps_ExprContext;
		ListCell   *lc;
		List	   *arbiterIndexes = NIL;

		/*
		 * If there is a list of arbiter indexes, map it to a list of indexes
		 * in the partition.  We do that by scanning the partition's index
		 * list and searching for ancestry relationships to each index in the
		 * ancestor table.
		 */
		if (list_length(resultRelInfo->ri_onConflictArbiterIndexes) > 0)
		{
			List	   *childIdxs;

			childIdxs = RelationGetIndexList(leaf_part_rri->ri_RelationDesc);

			foreach(lc, childIdxs)
			{
				Oid			childIdx = lfirst_oid(lc);
				List	   *ancestors;
				ListCell   *lc2;

				ancestors = get_partition_ancestors(childIdx);
				foreach(lc2, resultRelInfo->ri_onConflictArbiterIndexes)
				{
					if (list_member_oid(ancestors, lfirst_oid(lc2)))
						arbiterIndexes = lappend_oid(arbiterIndexes, childIdx);
				}
				list_free(ancestors);
			}
		}

		/*
		 * If the resulting lists are of inequal length, something is wrong.
		 * (This shouldn't happen, since arbiter index selection should not
		 * pick up an invalid index.)
		 */
		if (list_length(resultRelInfo->ri_onConflictArbiterIndexes) !=
			list_length(arbiterIndexes))
			elog(ERROR, "invalid arbiter index list");
		leaf_part_rri->ri_onConflictArbiterIndexes = arbiterIndexes;

		/*
		 * In the DO UPDATE case, we have some more state to initialize.
		 */
		if (node->onConflictAction == ONCONFLICT_UPDATE)
		{
			Assert(node->onConflictSet != NIL);
			Assert(resultRelInfo->ri_onConflict != NULL);

			/*
			 * If the partition's tuple descriptor matches exactly the root
			 * parent (the common case), we can simply re-use the parent's ON
			 * CONFLICT SET state, skipping a bunch of work.  Otherwise, we
			 * need to create state specific to this partition.
			 */
			if (map == NULL)
				leaf_part_rri->ri_onConflict = resultRelInfo->ri_onConflict;
			else
			{
				List	   *onconflset;
				TupleDesc	tupDesc;
				bool		found_whole_row;

				leaf_part_rri->ri_onConflict = makeNode(OnConflictSetState);

				/*
				 * Translate expressions in onConflictSet to account for
				 * different attribute numbers.  For that, map partition
				 * varattnos twice: first to catch the EXCLUDED
				 * pseudo-relation (INNER_VAR), and second to handle the main
				 * target relation (firstVarno).
				 */
				onconflset = (List *) copyObject((Node *) node->onConflictSet);
				if (part_attnos == NULL)
					part_attnos =
						convert_tuples_by_name_map(RelationGetDescr(partrel),
												   RelationGetDescr(firstResultRel),
												   gettext_noop("could not convert row type"));
				onconflset = (List *)
					map_variable_attnos((Node *) onconflset,
										INNER_VAR, 0,
										part_attnos,
										RelationGetDescr(firstResultRel)->natts,
										RelationGetForm(partrel)->reltype,
										&found_whole_row);
				/* We ignore the value of found_whole_row. */
				onconflset = (List *)
					map_variable_attnos((Node *) onconflset,
										firstVarno, 0,
										part_attnos,
										RelationGetDescr(firstResultRel)->natts,
										RelationGetForm(partrel)->reltype,
										&found_whole_row);
				/* We ignore the value of found_whole_row. */

				/* Finally, adjust this tlist to match the partition. */
				onconflset = adjust_partition_tlist(onconflset, map);

				/*
				 * Build UPDATE SET's projection info.  The user of this
				 * projection is responsible for setting the slot's tupdesc!
				 * We set aside a tupdesc that's good for the common case of a
				 * partition that's tupdesc-equal to the partitioned table;
				 * partitions of different tupdescs must generate their own.
				 */
				tupDesc = ExecTypeFromTL(onconflset, partrelDesc->tdhasoid);
				ExecSetSlotDescriptor(mtstate->mt_conflproj, tupDesc);
				leaf_part_rri->ri_onConflict->oc_ProjInfo =
					ExecBuildProjectionInfo(onconflset, econtext,
											mtstate->mt_conflproj,
											&mtstate->ps, partrelDesc);
				leaf_part_rri->ri_onConflict->oc_ProjTupdesc = tupDesc;

				/*
				 * If there is a WHERE clause, initialize state where it will
				 * be evaluated, mapping the attribute numbers appropriately.
				 * As with onConflictSet, we need to map partition varattnos
				 * to the partition's tupdesc.
				 */
				if (node->onConflictWhere)
				{
					List	   *clause;

					clause = copyObject((List *) node->onConflictWhere);
					clause = (List *)
						map_variable_attnos((Node *) clause,
											INNER_VAR, 0,
											part_attnos,
											RelationGetDescr(firstResultRel)->natts,
											RelationGetForm(partrel)->reltype,
											&found_whole_row);
					/* We ignore the value of found_whole_row. */
					clause = (List *)
						map_variable_attnos((Node *) clause,
											firstVarno, 0,
											part_attnos,
											RelationGetDescr(firstResultRel)->natts,
											RelationGetForm(partrel)->reltype,
											&found_whole_row);
					/* We ignore the value of found_whole_row. */
					leaf_part_rri->ri_onConflict->oc_WhereClause =
						ExecInitQual((List *) clause, &mtstate->ps);
				}
			}
		}
	}

	Assert(proute->partitions[partidx] == NULL);
	proute->partitions[partidx] = leaf_part_rri;

	MemoryContextSwitchTo(oldContext);

	return leaf_part_rri;
}

/*
 * ExecInitRoutingInfo
 *		Set up information needed for routing tuples to a leaf partition
 */
void
ExecInitRoutingInfo(ModifyTableState *mtstate,
					EState *estate,
					PartitionTupleRouting *proute,
					ResultRelInfo *partRelInfo,
					int partidx)
{
	MemoryContext oldContext;

	/*
	 * Switch into per-query memory context.
	 */
	oldContext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * Set up a tuple conversion map to convert a tuple routed to the
	 * partition from the parent's type to the partition's.
	 */
	proute->parent_child_tupconv_maps[partidx] =
		convert_tuples_by_name(RelationGetDescr(partRelInfo->ri_PartitionRoot),
							   RelationGetDescr(partRelInfo->ri_RelationDesc),
							   gettext_noop("could not convert row type"));

	/*
	 * If the partition is a foreign table, let the FDW init itself for
	 * routing tuples to the partition.
	 */
	if (partRelInfo->ri_FdwRoutine != NULL &&
		partRelInfo->ri_FdwRoutine->BeginForeignInsert != NULL)
		partRelInfo->ri_FdwRoutine->BeginForeignInsert(mtstate, partRelInfo);

	MemoryContextSwitchTo(oldContext);

	partRelInfo->ri_PartitionReadyForRouting = true;
}

/*
 * ExecSetupChildParentMapForLeaf -- Initialize the per-leaf-partition
 * child-to-root tuple conversion map array.
 *
 * This map is required for capturing transition tuples when the target table
 * is a partitioned table. For a tuple that is routed by an INSERT or UPDATE,
 * we need to convert it from the leaf partition to the target table
 * descriptor.
 */
void
ExecSetupChildParentMapForLeaf(PartitionTupleRouting *proute)
{
	Assert(proute != NULL);

	/*
	 * These array elements get filled up with maps on an on-demand basis.
	 * Initially just set all of them to NULL.
	 */
	proute->child_parent_tupconv_maps =
		(TupleConversionMap **) palloc0(sizeof(TupleConversionMap *) *
										proute->num_partitions);

	/* Same is the case for this array. All the values are set to false */
	proute->child_parent_map_not_required =
		(bool *) palloc0(sizeof(bool) * proute->num_partitions);
}

/*
 * TupConvMapForLeaf -- Get the tuple conversion map for a given leaf partition
 * index.
 */
TupleConversionMap *
TupConvMapForLeaf(PartitionTupleRouting *proute,
				  ResultRelInfo *rootRelInfo, int leaf_index)
{
	ResultRelInfo **resultRelInfos = proute->partitions;
	TupleConversionMap **map;
	TupleDesc	tupdesc;

	/* Don't call this if we're not supposed to be using this type of map. */
	Assert(proute->child_parent_tupconv_maps != NULL);

	/* If it's already known that we don't need a map, return NULL. */
	if (proute->child_parent_map_not_required[leaf_index])
		return NULL;

	/* If we've already got a map, return it. */
	map = &proute->child_parent_tupconv_maps[leaf_index];
	if (*map != NULL)
		return *map;

	/* No map yet; try to create one. */
	tupdesc = RelationGetDescr(resultRelInfos[leaf_index]->ri_RelationDesc);
	*map =
		convert_tuples_by_name(tupdesc,
							   RelationGetDescr(rootRelInfo->ri_RelationDesc),
							   gettext_noop("could not convert row type"));

	/* If it turns out no map is needed, remember for next time. */
	proute->child_parent_map_not_required[leaf_index] = (*map == NULL);

	return *map;
}

/*
 * ConvertPartitionTupleSlot -- convenience function for tuple conversion.
 * The tuple, if converted, is stored in new_slot, and *p_my_slot is
 * updated to point to it.  new_slot typically should be one of the
 * dedicated partition tuple slots. If map is NULL, *p_my_slot is not changed.
 *
 * Returns the converted tuple, unless map is NULL, in which case original
 * tuple is returned unmodified.
 */
HeapTuple
ConvertPartitionTupleSlot(TupleConversionMap *map,
						  HeapTuple tuple,
						  TupleTableSlot *new_slot,
						  TupleTableSlot **p_my_slot)
{
	if (!map)
		return tuple;

	tuple = do_convert_tuple(tuple, map);

	/*
	 * Change the partition tuple slot descriptor, as per converted tuple.
	 */
	*p_my_slot = new_slot;
	Assert(new_slot != NULL);
	ExecSetSlotDescriptor(new_slot, map->outdesc);
	ExecStoreTuple(tuple, new_slot, InvalidBuffer, true);

	return tuple;
}

/*
 * ExecCleanupTupleRouting -- Clean up objects allocated for partition tuple
 * routing.
 *
 * Close all the partitioned tables, leaf partitions, and their indices.
 */
void
ExecCleanupTupleRouting(ModifyTableState *mtstate,
						PartitionTupleRouting *proute)
{
	int			i;
	int			subplan_index = 0;

	/*
	 * Remember, proute->partition_dispatch_info[0] corresponds to the root
	 * partitioned table, which we must not try to close, because it is the
	 * main target table of the query that will be closed by callers such as
	 * ExecEndPlan() or DoCopy(). Also, tupslot is NULL for the root
	 * partitioned table.
	 */
	for (i = 1; i < proute->num_dispatch; i++)
	{
		PartitionDispatch pd = proute->partition_dispatch_info[i];

		heap_close(pd->reldesc, NoLock);
		ExecDropSingleTupleTableSlot(pd->tupslot);
	}

	for (i = 0; i < proute->num_partitions; i++)
	{
		ResultRelInfo *resultRelInfo = proute->partitions[i];

		/* skip further processsing for uninitialized partitions */
		if (resultRelInfo == NULL)
			continue;

		/* Allow any FDWs to shut down if they've been exercised */
		if (resultRelInfo->ri_PartitionReadyForRouting &&
			resultRelInfo->ri_FdwRoutine != NULL &&
			resultRelInfo->ri_FdwRoutine->EndForeignInsert != NULL)
			resultRelInfo->ri_FdwRoutine->EndForeignInsert(mtstate->ps.state,
														   resultRelInfo);

		/*
		 * If this result rel is one of the UPDATE subplan result rels, let
		 * ExecEndPlan() close it. For INSERT or COPY,
		 * proute->subplan_partition_offsets will always be NULL. Note that
		 * the subplan_partition_offsets array and the partitions array have
		 * the partitions in the same order. So, while we iterate over
		 * partitions array, we also iterate over the
		 * subplan_partition_offsets array in order to figure out which of the
		 * result rels are present in the UPDATE subplans.
		 */
		if (proute->subplan_partition_offsets &&
			subplan_index < proute->num_subplan_partition_offsets &&
			proute->subplan_partition_offsets[subplan_index] == i)
		{
			subplan_index++;
			continue;
		}

		ExecCloseIndices(resultRelInfo);
		heap_close(resultRelInfo->ri_RelationDesc, NoLock);
	}

	/* Release the standalone partition tuple descriptors, if any */
	if (proute->root_tuple_slot)
		ExecDropSingleTupleTableSlot(proute->root_tuple_slot);
	if (proute->partition_tuple_slot)
		ExecDropSingleTupleTableSlot(proute->partition_tuple_slot);
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
	 * The 'indexes' array is used when searching for a partition matching a
	 * given tuple.  The actual value we store here depends on whether the
	 * array element belongs to a leaf partition or a subpartitioned table.
	 * For leaf partitions we store the index into *leaf_part_oids, and for
	 * sub-partitioned tables we store a negative version of the index into
	 * the *pds list.  Both indexes are 0-based, but the first element of the
	 * *pds list is the root partition, so 0 always means the first leaf. When
	 * searching, if we see a negative value, the search must continue in the
	 * corresponding sub-partition; otherwise, we've identified the correct
	 * partition.
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
 * get_partition_for_tuple
 *		Finds partition of relation which accepts the partition key specified
 *		in values and isnull
 *
 * Return value is index of the partition (>= 0 and < partdesc->nparts) if one
 * found or -1 if none found.
 */
static int
get_partition_for_tuple(Relation relation, Datum *values, bool *isnull)
{
	int			bound_offset;
	int			part_index = -1;
	PartitionKey key = RelationGetPartitionKey(relation);
	PartitionDesc partdesc = RelationGetPartitionDesc(relation);
	PartitionBoundInfo boundinfo = partdesc->boundinfo;

	/* Route as appropriate based on partitioning strategy. */
	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			{
				int			greatest_modulus;
				uint64		rowHash;

				greatest_modulus = get_hash_partition_greatest_modulus(boundinfo);
				rowHash = compute_partition_hash_value(key->partnatts,
													   key->partsupfunc,
													   values, isnull);

				part_index = boundinfo->indexes[rowHash % greatest_modulus];
			}
			break;

		case PARTITION_STRATEGY_LIST:
			if (isnull[0])
			{
				if (partition_bound_accepts_nulls(boundinfo))
					part_index = boundinfo->null_index;
			}
			else
			{
				bool		equal = false;

				bound_offset = partition_list_bsearch(key->partsupfunc,
													  key->partcollation,
													  boundinfo,
													  values[0], &equal);
				if (bound_offset >= 0 && equal)
					part_index = boundinfo->indexes[bound_offset];
			}
			break;

		case PARTITION_STRATEGY_RANGE:
			{
				bool		equal = false,
							range_partkey_has_null = false;
				int			i;

				/*
				 * No range includes NULL, so this will be accepted by the
				 * default partition if there is one, and otherwise rejected.
				 */
				for (i = 0; i < key->partnatts; i++)
				{
					if (isnull[i])
					{
						range_partkey_has_null = true;
						break;
					}
				}

				if (!range_partkey_has_null)
				{
					bound_offset = partition_range_datum_bsearch(key->partsupfunc,
																 key->partcollation,
																 boundinfo,
																 key->partnatts,
																 values,
																 &equal);

					/*
					 * The bound at bound_offset is less than or equal to the
					 * tuple value, so the bound at offset+1 is the upper
					 * bound of the partition we're looking for, if there
					 * actually exists one.
					 */
					part_index = boundinfo->indexes[bound_offset + 1];
				}
			}
			break;

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
	}

	/*
	 * part_index < 0 means we failed to find a partition of this parent. Use
	 * the default partition, if there is one.
	 */
	if (part_index < 0)
		part_index = boundinfo->default_index;

	return part_index;
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

/*
 * adjust_partition_tlist
 *		Adjust the targetlist entries for a given partition to account for
 *		attribute differences between parent and the partition
 *
 * The expressions have already been fixed, but here we fix the list to make
 * target resnos match the partition's attribute numbers.  This results in a
 * copy of the original target list in which the entries appear in resno
 * order, including both the existing entries (that may have their resno
 * changed in-place) and the newly added entries for columns that don't exist
 * in the parent.
 *
 * Scribbles on the input tlist, so callers must make sure to make a copy
 * before passing it to us.
 */
static List *
adjust_partition_tlist(List *tlist, TupleConversionMap *map)
{
	List	   *new_tlist = NIL;
	TupleDesc	tupdesc = map->outdesc;
	AttrNumber *attrMap = map->attrMap;
	AttrNumber	attrno;

	for (attrno = 1; attrno <= tupdesc->natts; attrno++)
	{
		Form_pg_attribute att_tup = TupleDescAttr(tupdesc, attrno - 1);
		TargetEntry *tle;

		if (attrMap[attrno - 1] != InvalidAttrNumber)
		{
			Assert(!att_tup->attisdropped);

			/*
			 * Use the corresponding entry from the parent's tlist, adjusting
			 * the resno the match the partition's attno.
			 */
			tle = (TargetEntry *) list_nth(tlist, attrMap[attrno - 1] - 1);
			tle->resno = attrno;
		}
		else
		{
			Const	   *expr;

			/*
			 * For a dropped attribute in the partition, generate a dummy
			 * entry with resno matching the partition's attno.
			 */
			Assert(att_tup->attisdropped);
			expr = makeConst(INT4OID,
							 -1,
							 InvalidOid,
							 sizeof(int32),
							 (Datum) 0,
							 true,	/* isnull */
							 true /* byval */ );
			tle = makeTargetEntry((Expr *) expr,
								  attrno,
								  pstrdup(NameStr(att_tup->attname)),
								  false);
		}

		new_tlist = lappend(new_tlist, tle);
	}

	return new_tlist;
}

/*-------------------------------------------------------------------------
 * Run-Time Partition Pruning Support.
 *
 * The following series of functions exist to support the removal of unneeded
 * subplans for queries against partitioned tables.  The supporting functions
 * here are designed to work with any plan type which supports an arbitrary
 * number of subplans, e.g. Append, MergeAppend.
 *
 * When pruning involves comparison of a partition key to a constant, it's
 * done by the planner.  However, if we have a comparison to a non-constant
 * but not volatile expression, that presents an opportunity for run-time
 * pruning by the executor, allowing irrelevant partitions to be skipped
 * dynamically.
 *
 * We must distinguish expressions containing PARAM_EXEC Params from
 * expressions that don't contain those.  Even though a PARAM_EXEC Param is
 * considered to be a stable expression, it can change value from one plan
 * node scan to the next during query execution.  Stable comparison
 * expressions that don't involve such Params allow partition pruning to be
 * done once during executor startup.  Expressions that do involve such Params
 * require us to prune separately for each scan of the parent plan node.
 *
 * Note that pruning away unneeded subplans during executor startup has the
 * added benefit of not having to initialize the unneeded subplans at all.
 *
 *
 * Functions:
 *
 * ExecCreatePartitionPruneState:
 *		Creates the PartitionPruneState required by each of the two pruning
 *		functions.  Details stored include how to map the partition index
 *		returned by the partition pruning code into subplan indexes.
 *
 * ExecDestroyPartitionPruneState:
 *		Deletes a PartitionPruneState. Must be called during executor shutdown.
 *
 * ExecFindInitialMatchingSubPlans:
 *		Returns indexes of matching subplans.  Partition pruning is attempted
 *		without any evaluation of expressions containing PARAM_EXEC Params.
 *		This function must be called during executor startup for the parent
 *		plan before the subplans themselves are initialized.  Subplans which
 *		are found not to match by this function must be removed from the
 *		plan's list of subplans during execution, as this function performs a
 *		remap of the partition index to subplan index map and the newly
 *		created map provides indexes only for subplans which remain after
 *		calling this function.
 *
 * ExecFindMatchingSubPlans:
 *		Returns indexes of matching subplans after evaluating all available
 *		expressions.  This function can only be called during execution and
 *		must be called again each time the value of a Param listed in
 *		PartitionPruneState's 'execparamids' changes.
 *-------------------------------------------------------------------------
 */

/*
 * ExecCreatePartitionPruneState
 *		Build the data structure required for calling
 *		ExecFindInitialMatchingSubPlans and ExecFindMatchingSubPlans.
 *
 * 'planstate' is the parent plan node's execution state.
 *
 * 'partitionpruneinfo' is a PartitionPruneInfo as generated by
 * make_partition_pruneinfo.  Here we build a PartitionPruneState containing a
 * PartitionPruningData for each partitioning hierarchy (i.e., each sublist of
 * partitionpruneinfo->prune_infos), each of which contains a
 * PartitionedRelPruningData for each PartitionedRelPruneInfo appearing in
 * that sublist.  This two-level system is needed to keep from confusing the
 * different hierarchies when a UNION ALL contains multiple partitioned tables
 * as children.  The data stored in each PartitionedRelPruningData can be
 * re-used each time we re-evaluate which partitions match the pruning steps
 * provided in each PartitionedRelPruneInfo.
 */
PartitionPruneState *
ExecCreatePartitionPruneState(PlanState *planstate,
							  PartitionPruneInfo *partitionpruneinfo)
{
	PartitionPruneState *prunestate;
	int			n_part_hierarchies;
	ListCell   *lc;
	int			i;

	n_part_hierarchies = list_length(partitionpruneinfo->prune_infos);
	Assert(n_part_hierarchies > 0);

	/*
	 * Allocate the data structure
	 */
	prunestate = (PartitionPruneState *)
		palloc(offsetof(PartitionPruneState, partprunedata) +
			   sizeof(PartitionPruningData *) * n_part_hierarchies);

	prunestate->execparamids = NULL;
	/* other_subplans can change at runtime, so we need our own copy */
	prunestate->other_subplans = bms_copy(partitionpruneinfo->other_subplans);
	prunestate->do_initial_prune = false;	/* may be set below */
	prunestate->do_exec_prune = false;	/* may be set below */
	prunestate->num_partprunedata = n_part_hierarchies;

	/*
	 * Create a short-term memory context which we'll use when making calls to
	 * the partition pruning functions.  This avoids possible memory leaks,
	 * since the pruning functions call comparison functions that aren't under
	 * our control.
	 */
	prunestate->prune_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "Partition Prune",
							  ALLOCSET_DEFAULT_SIZES);

	i = 0;
	foreach(lc, partitionpruneinfo->prune_infos)
	{
		List	   *partrelpruneinfos = lfirst_node(List, lc);
		int			npartrelpruneinfos = list_length(partrelpruneinfos);
		PartitionPruningData *prunedata;
		ListCell   *lc2;
		int			j;

		prunedata = (PartitionPruningData *)
			palloc(offsetof(PartitionPruningData, partrelprunedata) +
				   npartrelpruneinfos * sizeof(PartitionedRelPruningData));
		prunestate->partprunedata[i] = prunedata;
		prunedata->num_partrelprunedata = npartrelpruneinfos;

		j = 0;
		foreach(lc2, partrelpruneinfos)
		{
			PartitionedRelPruneInfo *pinfo = lfirst_node(PartitionedRelPruneInfo, lc2);
			PartitionedRelPruningData *pprune = &prunedata->partrelprunedata[j];
			PartitionPruneContext *context = &pprune->context;
			PartitionDesc partdesc;
			PartitionKey partkey;
			int			partnatts;
			int			n_steps;
			ListCell   *lc3;

			/*
			 * We must copy the subplan_map rather than pointing directly to
			 * the plan's version, as we may end up making modifications to it
			 * later.
			 */
			pprune->subplan_map = palloc(sizeof(int) * pinfo->nparts);
			memcpy(pprune->subplan_map, pinfo->subplan_map,
				   sizeof(int) * pinfo->nparts);

			/* We can use the subpart_map verbatim, since we never modify it */
			pprune->subpart_map = pinfo->subpart_map;

			/* present_parts is also subject to later modification */
			pprune->present_parts = bms_copy(pinfo->present_parts);

			/*
			 * We need to hold a pin on the partitioned table's relcache entry
			 * so that we can rely on its copies of the table's partition key
			 * and partition descriptor.  We need not get a lock though; one
			 * should have been acquired already by InitPlan or
			 * ExecLockNonLeafAppendTables.
			 */
			context->partrel = relation_open(pinfo->reloid, NoLock);

			partkey = RelationGetPartitionKey(context->partrel);
			partdesc = RelationGetPartitionDesc(context->partrel);
			n_steps = list_length(pinfo->pruning_steps);

			context->strategy = partkey->strategy;
			context->partnatts = partnatts = partkey->partnatts;
			context->nparts = pinfo->nparts;
			context->boundinfo = partdesc->boundinfo;
			context->partcollation = partkey->partcollation;
			context->partsupfunc = partkey->partsupfunc;

			/* We'll look up type-specific support functions as needed */
			context->stepcmpfuncs = (FmgrInfo *)
				palloc0(sizeof(FmgrInfo) * n_steps * partnatts);

			context->ppccontext = CurrentMemoryContext;
			context->planstate = planstate;

			/* Initialize expression state for each expression we need */
			context->exprstates = (ExprState **)
				palloc0(sizeof(ExprState *) * n_steps * partnatts);
			foreach(lc3, pinfo->pruning_steps)
			{
				PartitionPruneStepOp *step = (PartitionPruneStepOp *) lfirst(lc3);
				ListCell   *lc4;
				int			keyno;

				/* not needed for other step kinds */
				if (!IsA(step, PartitionPruneStepOp))
					continue;

				Assert(list_length(step->exprs) <= partnatts);

				keyno = 0;
				foreach(lc4, step->exprs)
				{
					Expr	   *expr = (Expr *) lfirst(lc4);

					/* not needed for Consts */
					if (!IsA(expr, Const))
					{
						int			stateidx = PruneCxtStateIdx(partnatts,
																step->step.step_id,
																keyno);

						context->exprstates[stateidx] =
							ExecInitExpr(expr, context->planstate);
					}
					keyno++;
				}
			}

			/* Array is not modified at runtime, so just point to plan's copy */
			context->exprhasexecparam = pinfo->hasexecparam;

			pprune->pruning_steps = pinfo->pruning_steps;
			pprune->do_initial_prune = pinfo->do_initial_prune;
			pprune->do_exec_prune = pinfo->do_exec_prune;

			/* Record if pruning would be useful at any level */
			prunestate->do_initial_prune |= pinfo->do_initial_prune;
			prunestate->do_exec_prune |= pinfo->do_exec_prune;

			/*
			 * Accumulate the IDs of all PARAM_EXEC Params affecting the
			 * partitioning decisions at this plan node.
			 */
			prunestate->execparamids = bms_add_members(prunestate->execparamids,
													   pinfo->execparamids);

			j++;
		}
		i++;
	}

	return prunestate;
}

/*
 * ExecDestroyPartitionPruneState
 *		Release resources at plan shutdown.
 *
 * We don't bother to free any memory here, since the whole executor context
 * will be going away shortly.  We do need to release our relcache pins.
 */
void
ExecDestroyPartitionPruneState(PartitionPruneState *prunestate)
{
	PartitionPruningData **partprunedata = prunestate->partprunedata;
	int			i;

	for (i = 0; i < prunestate->num_partprunedata; i++)
	{
		PartitionPruningData *prunedata = partprunedata[i];
		PartitionedRelPruningData *pprune = prunedata->partrelprunedata;
		int			j;

		for (j = 0; j < prunedata->num_partrelprunedata; j++)
			relation_close(pprune[j].context.partrel, NoLock);
	}
}

/*
 * ExecFindInitialMatchingSubPlans
 *		Identify the set of subplans that cannot be eliminated by initial
 *		pruning (disregarding any pruning constraints involving PARAM_EXEC
 *		Params).  Also re-map the translation matrix which allows conversion
 *		of partition indexes into subplan indexes to account for the unneeded
 *		subplans having been removed.
 *
 * Must only be called once per 'prunestate', and only if initial pruning
 * is required.
 *
 * 'nsubplans' must be passed as the total number of unpruned subplans.
 */
Bitmapset *
ExecFindInitialMatchingSubPlans(PartitionPruneState *prunestate, int nsubplans)
{
	Bitmapset  *result = NULL;
	MemoryContext oldcontext;
	int			i;

	Assert(prunestate->do_initial_prune);

	/*
	 * Switch to a temp context to avoid leaking memory in the executor's
	 * memory context.
	 */
	oldcontext = MemoryContextSwitchTo(prunestate->prune_context);

	/*
	 * For each hierarchy, do the pruning tests, and add deletable subplans'
	 * indexes to "result".
	 */
	for (i = 0; i < prunestate->num_partprunedata; i++)
	{
		PartitionPruningData *prunedata;
		PartitionedRelPruningData *pprune;

		prunedata = prunestate->partprunedata[i];
		pprune = &prunedata->partrelprunedata[0];

		/* Perform pruning without using PARAM_EXEC Params */
		find_matching_subplans_recurse(prunedata, pprune, true, &result);

		/* Expression eval may have used space in node's ps_ExprContext too */
		ResetExprContext(pprune->context.planstate->ps_ExprContext);
	}

	MemoryContextSwitchTo(oldcontext);

	/* Copy result out of the temp context before we reset it */
	result = bms_copy(result);

	/* Add in any subplans that partition pruning didn't account for */
	result = bms_add_members(result, prunestate->other_subplans);

	MemoryContextReset(prunestate->prune_context);

	/*
	 * If any subplans were pruned, we must re-sequence the subplan indexes so
	 * that ExecFindMatchingSubPlans properly returns the indexes from the
	 * subplans which will remain after execution of this function.
	 */
	if (bms_num_members(result) < nsubplans)
	{
		int		   *new_subplan_indexes;
		Bitmapset  *new_other_subplans;
		int			i;
		int			newidx;

		/*
		 * First we must build a temporary array which maps old subplan
		 * indexes to new ones.  While we're at it, also recompute the
		 * other_subplans set, since indexes in it may change.
		 */
		new_subplan_indexes = (int *) palloc(sizeof(int) * nsubplans);
		new_other_subplans = NULL;
		newidx = 0;
		for (i = 0; i < nsubplans; i++)
		{
			if (bms_is_member(i, result))
				new_subplan_indexes[i] = newidx++;
			else
				new_subplan_indexes[i] = -1;	/* Newly pruned */

			if (bms_is_member(i, prunestate->other_subplans))
				new_other_subplans = bms_add_member(new_other_subplans,
													new_subplan_indexes[i]);
		}
		bms_free(prunestate->other_subplans);
		prunestate->other_subplans = new_other_subplans;

		/*
		 * Now we can update each PartitionedRelPruneInfo's subplan_map with
		 * new subplan indexes.  We must also recompute its present_parts
		 * bitmap.
		 */
		for (i = 0; i < prunestate->num_partprunedata; i++)
		{
			PartitionPruningData *prunedata = prunestate->partprunedata[i];
			int			j;

			/*
			 * Within each hierarchy, we perform this loop in back-to-front
			 * order so that we determine present_parts for the lowest-level
			 * partitioned tables first.  This way we can tell whether a
			 * sub-partitioned table's partitions were entirely pruned so we
			 * can exclude that from 'present_parts'.
			 */
			for (j = prunedata->num_partrelprunedata - 1; j >= 0; j--)
			{
				PartitionedRelPruningData *pprune = &prunedata->partrelprunedata[j];
				int			nparts = pprune->context.nparts;
				int			k;

				/* We just rebuild present_parts from scratch */
				bms_free(pprune->present_parts);
				pprune->present_parts = NULL;

				for (k = 0; k < nparts; k++)
				{
					int			oldidx = pprune->subplan_map[k];
					int			subidx;

					/*
					 * If this partition existed as a subplan then change the
					 * old subplan index to the new subplan index.  The new
					 * index may become -1 if the partition was pruned above,
					 * or it may just come earlier in the subplan list due to
					 * some subplans being removed earlier in the list.  If
					 * it's a subpartition, add it to present_parts unless
					 * it's entirely pruned.
					 */
					if (oldidx >= 0)
					{
						Assert(oldidx < nsubplans);
						pprune->subplan_map[k] = new_subplan_indexes[oldidx];

						if (new_subplan_indexes[oldidx] >= 0)
							pprune->present_parts =
								bms_add_member(pprune->present_parts, k);
					}
					else if ((subidx = pprune->subpart_map[k]) >= 0)
					{
						PartitionedRelPruningData *subprune;

						subprune = &prunedata->partrelprunedata[subidx];

						if (!bms_is_empty(subprune->present_parts))
							pprune->present_parts =
								bms_add_member(pprune->present_parts, k);
					}
				}
			}
		}

		pfree(new_subplan_indexes);
	}

	return result;
}

/*
 * ExecFindMatchingSubPlans
 *		Determine which subplans match the pruning steps detailed in
 *		'prunestate' for the current comparison expression values.
 *
 * Here we assume we may evaluate PARAM_EXEC Params.
 */
Bitmapset *
ExecFindMatchingSubPlans(PartitionPruneState *prunestate)
{
	Bitmapset  *result = NULL;
	MemoryContext oldcontext;
	int			i;

	/*
	 * Switch to a temp context to avoid leaking memory in the executor's
	 * memory context.
	 */
	oldcontext = MemoryContextSwitchTo(prunestate->prune_context);

	/*
	 * For each hierarchy, do the pruning tests, and add deletable subplans'
	 * indexes to "result".
	 */
	for (i = 0; i < prunestate->num_partprunedata; i++)
	{
		PartitionPruningData *prunedata;
		PartitionedRelPruningData *pprune;

		prunedata = prunestate->partprunedata[i];
		pprune = &prunedata->partrelprunedata[0];

		find_matching_subplans_recurse(prunedata, pprune, false, &result);

		/* Expression eval may have used space in node's ps_ExprContext too */
		ResetExprContext(pprune->context.planstate->ps_ExprContext);
	}

	MemoryContextSwitchTo(oldcontext);

	/* Copy result out of the temp context before we reset it */
	result = bms_copy(result);

	/* Add in any subplans that partition pruning didn't account for */
	result = bms_add_members(result, prunestate->other_subplans);

	MemoryContextReset(prunestate->prune_context);

	return result;
}

/*
 * find_matching_subplans_recurse
 *		Recursive worker function for ExecFindMatchingSubPlans and
 *		ExecFindInitialMatchingSubPlans
 *
 * Adds valid (non-prunable) subplan IDs to *validsubplans
 */
static void
find_matching_subplans_recurse(PartitionPruningData *prunedata,
							   PartitionedRelPruningData *pprune,
							   bool initial_prune,
							   Bitmapset **validsubplans)
{
	Bitmapset  *partset;
	int			i;

	/* Guard against stack overflow due to overly deep partition hierarchy. */
	check_stack_depth();

	/* Only prune if pruning would be useful at this level. */
	if (initial_prune ? pprune->do_initial_prune : pprune->do_exec_prune)
	{
		PartitionPruneContext *context = &pprune->context;

		/* Set whether we can evaluate PARAM_EXEC Params or not */
		context->evalexecparams = !initial_prune;

		partset = get_matching_partitions(context,
										  pprune->pruning_steps);
	}
	else
	{
		/*
		 * If no pruning is to be done, just include all partitions at this
		 * level.
		 */
		partset = pprune->present_parts;
	}

	/* Translate partset into subplan indexes */
	i = -1;
	while ((i = bms_next_member(partset, i)) >= 0)
	{
		if (pprune->subplan_map[i] >= 0)
			*validsubplans = bms_add_member(*validsubplans,
											pprune->subplan_map[i]);
		else
		{
			int			partidx = pprune->subpart_map[i];

			if (partidx >= 0)
				find_matching_subplans_recurse(prunedata,
											   &prunedata->partrelprunedata[partidx],
											   initial_prune, validsubplans);
			else
			{
				/*
				 * We get here if the planner already pruned all the sub-
				 * partitions for this partition.  Silently ignore this
				 * partition in this case.  The end result is the same: we
				 * would have pruned all partitions just the same, but we
				 * don't have any pruning steps to execute to verify this.
				 */
			}
		}
	}
}
