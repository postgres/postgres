/*-------------------------------------------------------------------------
 *
 * inherit.c
 *	  Routines to process child relations in inheritance trees
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/inherit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "partitioning/partdesc.h"
#include "partitioning/partprune.h"
#include "utils/rel.h"


static void expand_partitioned_rtentry(PlannerInfo *root, RelOptInfo *relinfo,
									   RangeTblEntry *parentrte,
									   Index parentRTindex, Relation parentrel,
									   Bitmapset *parent_updatedCols,
									   PlanRowMark *top_parentrc, LOCKMODE lockmode);
static void expand_single_inheritance_child(PlannerInfo *root,
											RangeTblEntry *parentrte,
											Index parentRTindex, Relation parentrel,
											PlanRowMark *top_parentrc, Relation childrel,
											RangeTblEntry **childrte_p,
											Index *childRTindex_p);
static Bitmapset *translate_col_privs(const Bitmapset *parent_privs,
									  List *translated_vars);
static Bitmapset *translate_col_privs_multilevel(PlannerInfo *root,
												 RelOptInfo *rel,
												 RelOptInfo *parent_rel,
												 Bitmapset *parent_cols);
static void expand_appendrel_subquery(PlannerInfo *root, RelOptInfo *rel,
									  RangeTblEntry *rte, Index rti);


/*
 * expand_inherited_rtentry
 *		Expand a rangetable entry that has the "inh" bit set.
 *
 * "inh" is only allowed in two cases: RELATION and SUBQUERY RTEs.
 *
 * "inh" on a plain RELATION RTE means that it is a partitioned table or the
 * parent of a traditional-inheritance set.  In this case we must add entries
 * for all the interesting child tables to the query's rangetable, and build
 * additional planner data structures for them, including RelOptInfos,
 * AppendRelInfos, and possibly PlanRowMarks.
 *
 * Note that the original RTE is considered to represent the whole inheritance
 * set.  In the case of traditional inheritance, the first of the generated
 * RTEs is an RTE for the same table, but with inh = false, to represent the
 * parent table in its role as a simple member of the inheritance set.  For
 * partitioning, we don't need a second RTE because the partitioned table
 * itself has no data and need not be scanned.
 *
 * "inh" on a SUBQUERY RTE means that it's the parent of a UNION ALL group,
 * which is treated as an appendrel similarly to inheritance cases; however,
 * we already made RTEs and AppendRelInfos for the subqueries.  We only need
 * to build RelOptInfos for them, which is done by expand_appendrel_subquery.
 */
void
expand_inherited_rtentry(PlannerInfo *root, RelOptInfo *rel,
						 RangeTblEntry *rte, Index rti)
{
	Oid			parentOID;
	Relation	oldrelation;
	LOCKMODE	lockmode;
	PlanRowMark *oldrc;
	bool		old_isParent = false;
	int			old_allMarkTypes = 0;

	Assert(rte->inh);			/* else caller error */

	if (rte->rtekind == RTE_SUBQUERY)
	{
		expand_appendrel_subquery(root, rel, rte, rti);
		return;
	}

	Assert(rte->rtekind == RTE_RELATION);

	parentOID = rte->relid;

	/*
	 * We used to check has_subclass() here, but there's no longer any need
	 * to, because subquery_planner already did.
	 */

	/*
	 * The rewriter should already have obtained an appropriate lock on each
	 * relation named in the query, so we can open the parent relation without
	 * locking it.  However, for each child relation we add to the query, we
	 * must obtain an appropriate lock, because this will be the first use of
	 * those relations in the parse/rewrite/plan pipeline.  Child rels should
	 * use the same lockmode as their parent.
	 */
	oldrelation = table_open(parentOID, NoLock);
	lockmode = rte->rellockmode;

	/*
	 * If parent relation is selected FOR UPDATE/SHARE, we need to mark its
	 * PlanRowMark as isParent = true, and generate a new PlanRowMark for each
	 * child.
	 */
	oldrc = get_plan_rowmark(root->rowMarks, rti);
	if (oldrc)
	{
		old_isParent = oldrc->isParent;
		oldrc->isParent = true;
		/* Save initial value of allMarkTypes before children add to it */
		old_allMarkTypes = oldrc->allMarkTypes;
	}

	/* Scan the inheritance set and expand it */
	if (oldrelation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		RTEPermissionInfo *perminfo;

		perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);

		/*
		 * Partitioned table, so set up for partitioning.
		 */
		Assert(rte->relkind == RELKIND_PARTITIONED_TABLE);

		/*
		 * Recursively expand and lock the partitions.  While at it, also
		 * extract the partition key columns of all the partitioned tables.
		 */
		expand_partitioned_rtentry(root, rel, rte, rti,
								   oldrelation,
								   perminfo->updatedCols,
								   oldrc, lockmode);
	}
	else
	{
		/*
		 * Ordinary table, so process traditional-inheritance children.  (Note
		 * that partitioned tables are not allowed to have inheritance
		 * children, so it's not possible for both cases to apply.)
		 */
		List	   *inhOIDs;
		ListCell   *l;

		/* Scan for all members of inheritance set, acquire needed locks */
		inhOIDs = find_all_inheritors(parentOID, lockmode, NULL);

		/*
		 * We used to special-case the situation where the table no longer has
		 * any children, by clearing rte->inh and exiting.  That no longer
		 * works, because this function doesn't get run until after decisions
		 * have been made that depend on rte->inh.  We have to treat such
		 * situations as normal inheritance.  The table itself should always
		 * have been found, though.
		 */
		Assert(inhOIDs != NIL);
		Assert(linitial_oid(inhOIDs) == parentOID);

		/* Expand simple_rel_array and friends to hold child objects. */
		expand_planner_arrays(root, list_length(inhOIDs));

		/*
		 * Expand inheritance children in the order the OIDs were returned by
		 * find_all_inheritors.
		 */
		foreach(l, inhOIDs)
		{
			Oid			childOID = lfirst_oid(l);
			Relation	newrelation;
			RangeTblEntry *childrte;
			Index		childRTindex;

			/* Open rel if needed; we already have required locks */
			if (childOID != parentOID)
				newrelation = table_open(childOID, NoLock);
			else
				newrelation = oldrelation;

			/*
			 * It is possible that the parent table has children that are temp
			 * tables of other backends.  We cannot safely access such tables
			 * (because of buffering issues), and the best thing to do seems
			 * to be to silently ignore them.
			 */
			if (childOID != parentOID && RELATION_IS_OTHER_TEMP(newrelation))
			{
				table_close(newrelation, lockmode);
				continue;
			}

			/* Create RTE and AppendRelInfo, plus PlanRowMark if needed. */
			expand_single_inheritance_child(root, rte, rti, oldrelation,
											oldrc, newrelation,
											&childrte, &childRTindex);

			/* Create the otherrel RelOptInfo too. */
			(void) build_simple_rel(root, childRTindex, rel);

			/* Close child relations, but keep locks */
			if (childOID != parentOID)
				table_close(newrelation, NoLock);
		}
	}

	/*
	 * Some children might require different mark types, which would've been
	 * reported into oldrc.  If so, add relevant entries to the top-level
	 * targetlist and update parent rel's reltarget.  This should match what
	 * preprocess_targetlist() would have added if the mark types had been
	 * requested originally.
	 *
	 * (Someday it might be useful to fold these resjunk columns into the
	 * row-identity-column management used for UPDATE/DELETE.  Today is not
	 * that day, however.)
	 */
	if (oldrc)
	{
		int			new_allMarkTypes = oldrc->allMarkTypes;
		Var		   *var;
		TargetEntry *tle;
		char		resname[32];
		List	   *newvars = NIL;

		/* Add TID junk Var if needed, unless we had it already */
		if (new_allMarkTypes & ~(1 << ROW_MARK_COPY) &&
			!(old_allMarkTypes & ~(1 << ROW_MARK_COPY)))
		{
			/* Need to fetch TID */
			var = makeVar(oldrc->rti,
						  SelfItemPointerAttributeNumber,
						  TIDOID,
						  -1,
						  InvalidOid,
						  0);
			snprintf(resname, sizeof(resname), "ctid%u", oldrc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(root->processed_tlist) + 1,
								  pstrdup(resname),
								  true);
			root->processed_tlist = lappend(root->processed_tlist, tle);
			newvars = lappend(newvars, var);
		}

		/* Add whole-row junk Var if needed, unless we had it already */
		if ((new_allMarkTypes & (1 << ROW_MARK_COPY)) &&
			!(old_allMarkTypes & (1 << ROW_MARK_COPY)))
		{
			var = makeWholeRowVar(planner_rt_fetch(oldrc->rti, root),
								  oldrc->rti,
								  0,
								  false);
			snprintf(resname, sizeof(resname), "wholerow%u", oldrc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(root->processed_tlist) + 1,
								  pstrdup(resname),
								  true);
			root->processed_tlist = lappend(root->processed_tlist, tle);
			newvars = lappend(newvars, var);
		}

		/* Add tableoid junk Var, unless we had it already */
		if (!old_isParent)
		{
			var = makeVar(oldrc->rti,
						  TableOidAttributeNumber,
						  OIDOID,
						  -1,
						  InvalidOid,
						  0);
			snprintf(resname, sizeof(resname), "tableoid%u", oldrc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(root->processed_tlist) + 1,
								  pstrdup(resname),
								  true);
			root->processed_tlist = lappend(root->processed_tlist, tle);
			newvars = lappend(newvars, var);
		}

		/*
		 * Add the newly added Vars to parent's reltarget.  We needn't worry
		 * about the children's reltargets, they'll be made later.
		 */
		add_vars_to_targetlist(root, newvars, bms_make_singleton(0));
	}

	table_close(oldrelation, NoLock);
}

/*
 * expand_partitioned_rtentry
 *		Recursively expand an RTE for a partitioned table.
 */
static void
expand_partitioned_rtentry(PlannerInfo *root, RelOptInfo *relinfo,
						   RangeTblEntry *parentrte,
						   Index parentRTindex, Relation parentrel,
						   Bitmapset *parent_updatedCols,
						   PlanRowMark *top_parentrc, LOCKMODE lockmode)
{
	PartitionDesc partdesc;
	Bitmapset  *live_parts;
	int			num_live_parts;
	int			i;

	check_stack_depth();

	Assert(parentrte->inh);

	partdesc = PartitionDirectoryLookup(root->glob->partition_directory,
										parentrel);

	/* A partitioned table should always have a partition descriptor. */
	Assert(partdesc);

	/*
	 * Note down whether any partition key cols are being updated. Though it's
	 * the root partitioned table's updatedCols we are interested in,
	 * parent_updatedCols provided by the caller contains the root partrel's
	 * updatedCols translated to match the attribute ordering of parentrel.
	 */
	if (!root->partColsUpdated)
		root->partColsUpdated =
			has_partition_attrs(parentrel, parent_updatedCols, NULL);

	/* Nothing further to do here if there are no partitions. */
	if (partdesc->nparts == 0)
		return;

	/*
	 * Perform partition pruning using restriction clauses assigned to parent
	 * relation.  live_parts will contain PartitionDesc indexes of partitions
	 * that survive pruning.  Below, we will initialize child objects for the
	 * surviving partitions.
	 */
	relinfo->live_parts = live_parts = prune_append_rel_partitions(relinfo);

	/* Expand simple_rel_array and friends to hold child objects. */
	num_live_parts = bms_num_members(live_parts);
	if (num_live_parts > 0)
		expand_planner_arrays(root, num_live_parts);

	/*
	 * We also store partition RelOptInfo pointers in the parent relation.
	 * Since we're palloc0'ing, slots corresponding to pruned partitions will
	 * contain NULL.
	 */
	Assert(relinfo->part_rels == NULL);
	relinfo->part_rels = (RelOptInfo **)
		palloc0(relinfo->nparts * sizeof(RelOptInfo *));

	/*
	 * Create a child RTE for each live partition.  Note that unlike
	 * traditional inheritance, we don't need a child RTE for the partitioned
	 * table itself, because it's not going to be scanned.
	 */
	i = -1;
	while ((i = bms_next_member(live_parts, i)) >= 0)
	{
		Oid			childOID = partdesc->oids[i];
		Relation	childrel;
		RangeTblEntry *childrte;
		Index		childRTindex;
		RelOptInfo *childrelinfo;

		/*
		 * Open rel, acquiring required locks.  If a partition was recently
		 * detached and subsequently dropped, then opening it will fail.  In
		 * this case, behave as though the partition had been pruned.
		 */
		childrel = try_table_open(childOID, lockmode);
		if (childrel == NULL)
		{
			relinfo->live_parts = bms_del_member(relinfo->live_parts, i);
			continue;
		}

		/*
		 * Temporary partitions belonging to other sessions should have been
		 * disallowed at definition, but for paranoia's sake, let's double
		 * check.
		 */
		if (RELATION_IS_OTHER_TEMP(childrel))
			elog(ERROR, "temporary relation from another session found as partition");

		/* Create RTE and AppendRelInfo, plus PlanRowMark if needed. */
		expand_single_inheritance_child(root, parentrte, parentRTindex,
										parentrel, top_parentrc, childrel,
										&childrte, &childRTindex);

		/* Create the otherrel RelOptInfo too. */
		childrelinfo = build_simple_rel(root, childRTindex, relinfo);
		relinfo->part_rels[i] = childrelinfo;
		relinfo->all_partrels = bms_add_members(relinfo->all_partrels,
												childrelinfo->relids);

		/* If this child is itself partitioned, recurse */
		if (childrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			AppendRelInfo *appinfo = root->append_rel_array[childRTindex];
			Bitmapset  *child_updatedCols;

			child_updatedCols = translate_col_privs(parent_updatedCols,
													appinfo->translated_vars);

			expand_partitioned_rtentry(root, childrelinfo,
									   childrte, childRTindex,
									   childrel,
									   child_updatedCols,
									   top_parentrc, lockmode);
		}

		/* Close child relation, but keep locks */
		table_close(childrel, NoLock);
	}
}

/*
 * expand_single_inheritance_child
 *		Build a RangeTblEntry and an AppendRelInfo, plus maybe a PlanRowMark.
 *
 * We now expand the partition hierarchy level by level, creating a
 * corresponding hierarchy of AppendRelInfos and RelOptInfos, where each
 * partitioned descendant acts as a parent of its immediate partitions.
 * (This is a difference from what older versions of PostgreSQL did and what
 * is still done in the case of table inheritance for unpartitioned tables,
 * where the hierarchy is flattened during RTE expansion.)
 *
 * PlanRowMarks still carry the top-parent's RTI, and the top-parent's
 * allMarkTypes field still accumulates values from all descendents.
 *
 * "parentrte" and "parentRTindex" are immediate parent's RTE and
 * RTI. "top_parentrc" is top parent's PlanRowMark.
 *
 * The child RangeTblEntry and its RTI are returned in "childrte_p" and
 * "childRTindex_p" resp.
 */
static void
expand_single_inheritance_child(PlannerInfo *root, RangeTblEntry *parentrte,
								Index parentRTindex, Relation parentrel,
								PlanRowMark *top_parentrc, Relation childrel,
								RangeTblEntry **childrte_p,
								Index *childRTindex_p)
{
	Query	   *parse = root->parse;
	Oid			parentOID PG_USED_FOR_ASSERTS_ONLY =
		RelationGetRelid(parentrel);
	Oid			childOID = RelationGetRelid(childrel);
	RangeTblEntry *childrte;
	Index		childRTindex;
	AppendRelInfo *appinfo;
	TupleDesc	child_tupdesc;
	List	   *parent_colnames;
	List	   *child_colnames;

	/*
	 * Build an RTE for the child, and attach to query's rangetable list. We
	 * copy most scalar fields of the parent's RTE, but replace relation OID,
	 * relkind, and inh for the child.  Set the child's securityQuals to
	 * empty, because we only want to apply the parent's RLS conditions
	 * regardless of what RLS properties individual children may have. (This
	 * is an intentional choice to make inherited RLS work like regular
	 * permissions checks.) The parent securityQuals will be propagated to
	 * children along with other base restriction clauses, so we don't need to
	 * do it here.  Other infrastructure of the parent RTE has to be
	 * translated to match the child table's column ordering, which we do
	 * below, so a "flat" copy is sufficient to start with.
	 */
	childrte = makeNode(RangeTblEntry);
	memcpy(childrte, parentrte, sizeof(RangeTblEntry));
	Assert(parentrte->rtekind == RTE_RELATION); /* else this is dubious */
	childrte->relid = childOID;
	childrte->relkind = childrel->rd_rel->relkind;
	/* A partitioned child will need to be expanded further. */
	if (childrte->relkind == RELKIND_PARTITIONED_TABLE)
	{
		Assert(childOID != parentOID);
		childrte->inh = true;
	}
	else
		childrte->inh = false;
	childrte->securityQuals = NIL;

	/* No permission checking for child RTEs. */
	childrte->perminfoindex = 0;

	/* Link not-yet-fully-filled child RTE into data structures */
	parse->rtable = lappend(parse->rtable, childrte);
	childRTindex = list_length(parse->rtable);
	*childrte_p = childrte;
	*childRTindex_p = childRTindex;

	/*
	 * Build an AppendRelInfo struct for each parent/child pair.
	 */
	appinfo = make_append_rel_info(parentrel, childrel,
								   parentRTindex, childRTindex);
	root->append_rel_list = lappend(root->append_rel_list, appinfo);

	/* tablesample is probably null, but copy it */
	childrte->tablesample = copyObject(parentrte->tablesample);

	/*
	 * Construct an alias clause for the child, which we can also use as eref.
	 * This is important so that EXPLAIN will print the right column aliases
	 * for child-table columns.  (Since ruleutils.c doesn't have any easy way
	 * to reassociate parent and child columns, we must get the child column
	 * aliases right to start with.  Note that setting childrte->alias forces
	 * ruleutils.c to use these column names, which it otherwise would not.)
	 */
	child_tupdesc = RelationGetDescr(childrel);
	parent_colnames = parentrte->eref->colnames;
	child_colnames = NIL;
	for (int cattno = 0; cattno < child_tupdesc->natts; cattno++)
	{
		Form_pg_attribute att = TupleDescAttr(child_tupdesc, cattno);
		const char *attname;

		if (att->attisdropped)
		{
			/* Always insert an empty string for a dropped column */
			attname = "";
		}
		else if (appinfo->parent_colnos[cattno] > 0 &&
				 appinfo->parent_colnos[cattno] <= list_length(parent_colnames))
		{
			/* Duplicate the query-assigned name for the parent column */
			attname = strVal(list_nth(parent_colnames,
									  appinfo->parent_colnos[cattno] - 1));
		}
		else
		{
			/* New column, just use its real name */
			attname = NameStr(att->attname);
		}
		child_colnames = lappend(child_colnames, makeString(pstrdup(attname)));
	}

	/*
	 * We just duplicate the parent's table alias name for each child.  If the
	 * plan gets printed, ruleutils.c has to sort out unique table aliases to
	 * use, which it can handle.
	 */
	childrte->alias = childrte->eref = makeAlias(parentrte->eref->aliasname,
												 child_colnames);

	/*
	 * Store the RTE and appinfo in the respective PlannerInfo arrays, which
	 * the caller must already have allocated space for.
	 */
	Assert(childRTindex < root->simple_rel_array_size);
	Assert(root->simple_rte_array[childRTindex] == NULL);
	root->simple_rte_array[childRTindex] = childrte;
	Assert(root->append_rel_array[childRTindex] == NULL);
	root->append_rel_array[childRTindex] = appinfo;

	/*
	 * Build a PlanRowMark if parent is marked FOR UPDATE/SHARE.
	 */
	if (top_parentrc)
	{
		PlanRowMark *childrc = makeNode(PlanRowMark);

		childrc->rti = childRTindex;
		childrc->prti = top_parentrc->rti;
		childrc->rowmarkId = top_parentrc->rowmarkId;
		/* Reselect rowmark type, because relkind might not match parent */
		childrc->markType = select_rowmark_type(childrte,
												top_parentrc->strength);
		childrc->allMarkTypes = (1 << childrc->markType);
		childrc->strength = top_parentrc->strength;
		childrc->waitPolicy = top_parentrc->waitPolicy;

		/*
		 * We mark RowMarks for partitioned child tables as parent RowMarks so
		 * that the executor ignores them (except their existence means that
		 * the child tables will be locked using the appropriate mode).
		 */
		childrc->isParent = (childrte->relkind == RELKIND_PARTITIONED_TABLE);

		/* Include child's rowmark type in top parent's allMarkTypes */
		top_parentrc->allMarkTypes |= childrc->allMarkTypes;

		root->rowMarks = lappend(root->rowMarks, childrc);
	}

	/*
	 * If we are creating a child of the query target relation (only possible
	 * in UPDATE/DELETE/MERGE), add it to all_result_relids, as well as
	 * leaf_result_relids if appropriate, and make sure that we generate
	 * required row-identity data.
	 */
	if (bms_is_member(parentRTindex, root->all_result_relids))
	{
		/* OK, record the child as a result rel too. */
		root->all_result_relids = bms_add_member(root->all_result_relids,
												 childRTindex);

		/* Non-leaf partitions don't need any row identity info. */
		if (childrte->relkind != RELKIND_PARTITIONED_TABLE)
		{
			Var		   *rrvar;

			root->leaf_result_relids = bms_add_member(root->leaf_result_relids,
													  childRTindex);

			/*
			 * If we have any child target relations, assume they all need to
			 * generate a junk "tableoid" column.  (If only one child survives
			 * pruning, we wouldn't really need this, but it's not worth
			 * thrashing about to avoid it.)
			 */
			rrvar = makeVar(childRTindex,
							TableOidAttributeNumber,
							OIDOID,
							-1,
							InvalidOid,
							0);
			add_row_identity_var(root, rrvar, childRTindex, "tableoid");

			/* Register any row-identity columns needed by this child. */
			add_row_identity_columns(root, childRTindex,
									 childrte, childrel);
		}
	}
}

/*
 * get_rel_all_updated_cols
 * 		Returns the set of columns of a given "simple" relation that are
 * 		updated by this query.
 */
Bitmapset *
get_rel_all_updated_cols(PlannerInfo *root, RelOptInfo *rel)
{
	Index		relid;
	RangeTblEntry *rte;
	RTEPermissionInfo *perminfo;
	Bitmapset  *updatedCols,
			   *extraUpdatedCols;

	Assert(root->parse->commandType == CMD_UPDATE);
	Assert(IS_SIMPLE_REL(rel));

	/*
	 * We obtain updatedCols for the query's result relation.  Then, if
	 * necessary, we map it to the column numbers of the relation for which
	 * they were requested.
	 */
	relid = root->parse->resultRelation;
	rte = planner_rt_fetch(relid, root);
	perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);

	updatedCols = perminfo->updatedCols;

	if (rel->relid != relid)
	{
		RelOptInfo *top_parent_rel = find_base_rel(root, relid);

		Assert(IS_OTHER_REL(rel));

		updatedCols = translate_col_privs_multilevel(root, rel, top_parent_rel,
													 updatedCols);
	}

	/*
	 * Now we must check to see if there are any generated columns that depend
	 * on the updatedCols, and add them to the result.
	 */
	extraUpdatedCols = get_dependent_generated_columns(root, rel->relid,
													   updatedCols);

	return bms_union(updatedCols, extraUpdatedCols);
}

/*
 * translate_col_privs
 *	  Translate a bitmapset representing per-column privileges from the
 *	  parent rel's attribute numbering to the child's.
 *
 * The only surprise here is that we don't translate a parent whole-row
 * reference into a child whole-row reference.  That would mean requiring
 * permissions on all child columns, which is overly strict, since the
 * query is really only going to reference the inherited columns.  Instead
 * we set the per-column bits for all inherited columns.
 */
static Bitmapset *
translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars)
{
	Bitmapset  *child_privs = NULL;
	bool		whole_row;
	int			attno;
	ListCell   *lc;

	/* System attributes have the same numbers in all tables */
	for (attno = FirstLowInvalidHeapAttributeNumber + 1; attno < 0; attno++)
	{
		if (bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
										 attno - FirstLowInvalidHeapAttributeNumber);
	}

	/* Check if parent has whole-row reference */
	whole_row = bms_is_member(InvalidAttrNumber - FirstLowInvalidHeapAttributeNumber,
							  parent_privs);

	/* And now translate the regular user attributes, using the vars list */
	attno = InvalidAttrNumber;
	foreach(lc, translated_vars)
	{
		Var		   *var = lfirst_node(Var, lc);

		attno++;
		if (var == NULL)		/* ignore dropped columns */
			continue;
		if (whole_row ||
			bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
										 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	return child_privs;
}

/*
 * translate_col_privs_multilevel
 *		Recursively translates the column numbers contained in 'parent_cols'
 *		to the column numbers of a descendant relation given by 'rel'
 *
 * Note that because this is based on translate_col_privs, it will expand
 * a whole-row reference into all inherited columns.  This is not an issue
 * for current usages, but beware.
 */
static Bitmapset *
translate_col_privs_multilevel(PlannerInfo *root, RelOptInfo *rel,
							   RelOptInfo *parent_rel,
							   Bitmapset *parent_cols)
{
	AppendRelInfo *appinfo;

	/* Fast path for easy case. */
	if (parent_cols == NULL)
		return NULL;

	/* Recurse if immediate parent is not the top parent. */
	if (rel->parent != parent_rel)
	{
		if (rel->parent)
			parent_cols = translate_col_privs_multilevel(root, rel->parent,
														 parent_rel,
														 parent_cols);
		else
			elog(ERROR, "rel with relid %u is not a child rel", rel->relid);
	}

	/* Now translate for this child. */
	Assert(root->append_rel_array != NULL);
	appinfo = root->append_rel_array[rel->relid];
	Assert(appinfo != NULL);

	return translate_col_privs(parent_cols, appinfo->translated_vars);
}

/*
 * expand_appendrel_subquery
 *		Add "other rel" RelOptInfos for the children of an appendrel baserel
 *
 * "rel" is a subquery relation that has the rte->inh flag set, meaning it
 * is a UNION ALL subquery that's been flattened into an appendrel, with
 * child subqueries listed in root->append_rel_list.  We need to build
 * a RelOptInfo for each child relation so that we can plan scans on them.
 */
static void
expand_appendrel_subquery(PlannerInfo *root, RelOptInfo *rel,
						  RangeTblEntry *rte, Index rti)
{
	ListCell   *l;

	foreach(l, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);
		Index		childRTindex = appinfo->child_relid;
		RangeTblEntry *childrte;
		RelOptInfo *childrel;

		/* append_rel_list contains all append rels; ignore others */
		if (appinfo->parent_relid != rti)
			continue;

		/* find the child RTE, which should already exist */
		Assert(childRTindex < root->simple_rel_array_size);
		childrte = root->simple_rte_array[childRTindex];
		Assert(childrte != NULL);

		/* Build the child RelOptInfo. */
		childrel = build_simple_rel(root, childRTindex, rel);

		/* Child may itself be an inherited rel, either table or subquery. */
		if (childrte->inh)
			expand_inherited_rtentry(root, childrel, childrte, childRTindex);
	}
}


/*
 * apply_child_basequals
 *		Populate childrel's base restriction quals from parent rel's quals,
 *		translating Vars using appinfo and re-checking for quals which are
 *		constant-TRUE or constant-FALSE when applied to this child relation.
 *
 * If any of the resulting clauses evaluate to constant false or NULL, we
 * return false and don't apply any quals.  Caller should mark the relation as
 * a dummy rel in this case, since it doesn't need to be scanned.  Constant
 * true quals are ignored.
 */
bool
apply_child_basequals(PlannerInfo *root, RelOptInfo *parentrel,
					  RelOptInfo *childrel, RangeTblEntry *childRTE,
					  AppendRelInfo *appinfo)
{
	List	   *childquals;
	Index		cq_min_security;
	ListCell   *lc;

	/*
	 * The child rel's targetlist might contain non-Var expressions, which
	 * means that substitution into the quals could produce opportunities for
	 * const-simplification, and perhaps even pseudoconstant quals. Therefore,
	 * transform each RestrictInfo separately to see if it reduces to a
	 * constant or pseudoconstant.  (We must process them separately to keep
	 * track of the security level of each qual.)
	 */
	childquals = NIL;
	cq_min_security = UINT_MAX;
	foreach(lc, parentrel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node	   *childqual;
		ListCell   *lc2;

		Assert(IsA(rinfo, RestrictInfo));
		childqual = adjust_appendrel_attrs(root,
										   (Node *) rinfo->clause,
										   1, &appinfo);
		childqual = eval_const_expressions(root, childqual);
		/* check for flat-out constant */
		if (childqual && IsA(childqual, Const))
		{
			if (((Const *) childqual)->constisnull ||
				!DatumGetBool(((Const *) childqual)->constvalue))
			{
				/* Restriction reduces to constant FALSE or NULL */
				return false;
			}
			/* Restriction reduces to constant TRUE, so drop it */
			continue;
		}
		/* might have gotten an AND clause, if so flatten it */
		foreach(lc2, make_ands_implicit((Expr *) childqual))
		{
			Node	   *onecq = (Node *) lfirst(lc2);
			bool		pseudoconstant;
			RestrictInfo *childrinfo;

			/* check for pseudoconstant (no Vars or volatile functions) */
			pseudoconstant =
				!contain_vars_of_level(onecq, 0) &&
				!contain_volatile_functions(onecq);
			if (pseudoconstant)
			{
				/* tell createplan.c to check for gating quals */
				root->hasPseudoConstantQuals = true;
			}
			/* reconstitute RestrictInfo with appropriate properties */
			childrinfo = make_restrictinfo(root,
										   (Expr *) onecq,
										   rinfo->is_pushed_down,
										   rinfo->has_clone,
										   rinfo->is_clone,
										   pseudoconstant,
										   rinfo->security_level,
										   NULL, NULL, NULL);

			/* Restriction is proven always false */
			if (restriction_is_always_false(root, childrinfo))
				return false;
			/* Restriction is proven always true, so drop it */
			if (restriction_is_always_true(root, childrinfo))
				continue;

			childquals = lappend(childquals, childrinfo);
			/* track minimum security level among child quals */
			cq_min_security = Min(cq_min_security, rinfo->security_level);
		}
	}

	/*
	 * In addition to the quals inherited from the parent, we might have
	 * securityQuals associated with this particular child node.  (Currently
	 * this can only happen in appendrels originating from UNION ALL;
	 * inheritance child tables don't have their own securityQuals, see
	 * expand_single_inheritance_child().)  Pull any such securityQuals up
	 * into the baserestrictinfo for the child.  This is similar to
	 * process_security_barrier_quals() for the parent rel, except that we
	 * can't make any general deductions from such quals, since they don't
	 * hold for the whole appendrel.
	 */
	if (childRTE->securityQuals)
	{
		Index		security_level = 0;

		foreach(lc, childRTE->securityQuals)
		{
			List	   *qualset = (List *) lfirst(lc);
			ListCell   *lc2;

			foreach(lc2, qualset)
			{
				Expr	   *qual = (Expr *) lfirst(lc2);

				/* not likely that we'd see constants here, so no check */
				childquals = lappend(childquals,
									 make_restrictinfo(root, qual,
													   true,
													   false, false,
													   false,
													   security_level,
													   NULL, NULL, NULL));
				cq_min_security = Min(cq_min_security, security_level);
			}
			security_level++;
		}
		Assert(security_level <= root->qual_security_level);
	}

	/*
	 * OK, we've got all the baserestrictinfo quals for this child.
	 */
	childrel->baserestrictinfo = childquals;
	childrel->baserestrict_min_security = cq_min_security;

	return true;
}
