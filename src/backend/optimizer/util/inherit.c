/*-------------------------------------------------------------------------
 *
 * inherit.c
 *	  Routines to process child relations in inheritance trees
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/inherit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "miscadmin.h"
#include "optimizer/appendinfo.h"
#include "optimizer/inherit.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "partitioning/partdesc.h"
#include "utils/rel.h"


static void expand_inherited_rtentry(PlannerInfo *root, RangeTblEntry *rte,
						 Index rti);
static void expand_partitioned_rtentry(PlannerInfo *root,
						   RangeTblEntry *parentrte,
						   Index parentRTindex, Relation parentrel,
						   PlanRowMark *top_parentrc, LOCKMODE lockmode,
						   List **appinfos);
static void expand_single_inheritance_child(PlannerInfo *root,
								RangeTblEntry *parentrte,
								Index parentRTindex, Relation parentrel,
								PlanRowMark *top_parentrc, Relation childrel,
								List **appinfos, RangeTblEntry **childrte_p,
								Index *childRTindex_p);
static Bitmapset *translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars);


/*
 * expand_inherited_tables
 *		Expand each rangetable entry that represents an inheritance set
 *		into an "append relation".  At the conclusion of this process,
 *		the "inh" flag is set in all and only those RTEs that are append
 *		relation parents.
 */
void
expand_inherited_tables(PlannerInfo *root)
{
	Index		nrtes;
	Index		rti;
	ListCell   *rl;

	/*
	 * expand_inherited_rtentry may add RTEs to parse->rtable. The function is
	 * expected to recursively handle any RTEs that it creates with inh=true.
	 * So just scan as far as the original end of the rtable list.
	 */
	nrtes = list_length(root->parse->rtable);
	rl = list_head(root->parse->rtable);
	for (rti = 1; rti <= nrtes; rti++)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rl);

		expand_inherited_rtentry(root, rte, rti);
		rl = lnext(rl);
	}
}

/*
 * expand_inherited_rtentry
 *		Check whether a rangetable entry represents an inheritance set.
 *		If so, add entries for all the child tables to the query's
 *		rangetable, and build AppendRelInfo nodes for all the child tables
 *		and add them to root->append_rel_list.  If not, clear the entry's
 *		"inh" flag to prevent later code from looking for AppendRelInfos.
 *
 * Note that the original RTE is considered to represent the whole
 * inheritance set.  The first of the generated RTEs is an RTE for the same
 * table, but with inh = false, to represent the parent table in its role
 * as a simple member of the inheritance set.
 *
 * A childless table is never considered to be an inheritance set. For
 * regular inheritance, a parent RTE must always have at least two associated
 * AppendRelInfos: one corresponding to the parent table as a simple member of
 * inheritance set and one or more corresponding to the actual children.
 * Since a partitioned table is not scanned, it might have only one associated
 * AppendRelInfo.
 */
static void
expand_inherited_rtentry(PlannerInfo *root, RangeTblEntry *rte, Index rti)
{
	Oid			parentOID;
	PlanRowMark *oldrc;
	Relation	oldrelation;
	LOCKMODE	lockmode;
	List	   *inhOIDs;
	ListCell   *l;

	/* Does RT entry allow inheritance? */
	if (!rte->inh)
		return;
	/* Ignore any already-expanded UNION ALL nodes */
	if (rte->rtekind != RTE_RELATION)
	{
		Assert(rte->rtekind == RTE_SUBQUERY);
		return;
	}
	/* Fast path for common case of childless table */
	parentOID = rte->relid;
	if (!has_subclass(parentOID))
	{
		/* Clear flag before returning */
		rte->inh = false;
		return;
	}

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
		oldrc->isParent = true;

	/* Scan the inheritance set and expand it */
	if (oldrelation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		Assert(rte->relkind == RELKIND_PARTITIONED_TABLE);

		if (root->glob->partition_directory == NULL)
			root->glob->partition_directory =
				CreatePartitionDirectory(CurrentMemoryContext);

		/*
		 * If this table has partitions, recursively expand and lock them.
		 * While at it, also extract the partition key columns of all the
		 * partitioned tables.
		 */
		expand_partitioned_rtentry(root, rte, rti, oldrelation, oldrc,
								   lockmode, &root->append_rel_list);
	}
	else
	{
		List	   *appinfos = NIL;
		RangeTblEntry *childrte;
		Index		childRTindex;

		/* Scan for all members of inheritance set, acquire needed locks */
		inhOIDs = find_all_inheritors(parentOID, lockmode, NULL);

		/*
		 * Check that there's at least one descendant, else treat as no-child
		 * case.  This could happen despite above has_subclass() check, if the
		 * table once had a child but no longer does.
		 */
		if (list_length(inhOIDs) < 2)
		{
			/* Clear flag before returning */
			rte->inh = false;
			heap_close(oldrelation, NoLock);
			return;
		}

		/*
		 * This table has no partitions.  Expand any plain inheritance
		 * children in the order the OIDs were returned by
		 * find_all_inheritors.
		 */
		foreach(l, inhOIDs)
		{
			Oid			childOID = lfirst_oid(l);
			Relation	newrelation;

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

			expand_single_inheritance_child(root, rte, rti, oldrelation, oldrc,
											newrelation,
											&appinfos, &childrte,
											&childRTindex);

			/* Close child relations, but keep locks */
			if (childOID != parentOID)
				table_close(newrelation, NoLock);
		}

		/*
		 * If all the children were temp tables, pretend it's a
		 * non-inheritance situation; we don't need Append node in that case.
		 * The duplicate RTE we added for the parent table is harmless, so we
		 * don't bother to get rid of it; ditto for the useless PlanRowMark
		 * node.
		 */
		if (list_length(appinfos) < 2)
			rte->inh = false;
		else
			root->append_rel_list = list_concat(root->append_rel_list,
												appinfos);

	}

	table_close(oldrelation, NoLock);
}

/*
 * expand_partitioned_rtentry
 *		Recursively expand an RTE for a partitioned table.
 */
static void
expand_partitioned_rtentry(PlannerInfo *root, RangeTblEntry *parentrte,
						   Index parentRTindex, Relation parentrel,
						   PlanRowMark *top_parentrc, LOCKMODE lockmode,
						   List **appinfos)
{
	int			i;
	RangeTblEntry *childrte;
	Index		childRTindex;
	PartitionDesc partdesc;

	partdesc = PartitionDirectoryLookup(root->glob->partition_directory,
										parentrel);

	check_stack_depth();

	/* A partitioned table should always have a partition descriptor. */
	Assert(partdesc);

	Assert(parentrte->inh);

	/*
	 * Note down whether any partition key cols are being updated. Though it's
	 * the root partitioned table's updatedCols we are interested in, we
	 * instead use parentrte to get the updatedCols. This is convenient
	 * because parentrte already has the root partrel's updatedCols translated
	 * to match the attribute ordering of parentrel.
	 */
	if (!root->partColsUpdated)
		root->partColsUpdated =
			has_partition_attrs(parentrel, parentrte->updatedCols, NULL);

	/* First expand the partitioned table itself. */
	expand_single_inheritance_child(root, parentrte, parentRTindex, parentrel,
									top_parentrc, parentrel,
									appinfos, &childrte, &childRTindex);

	/*
	 * If the partitioned table has no partitions, treat this as the
	 * non-inheritance case.
	 */
	if (partdesc->nparts == 0)
	{
		parentrte->inh = false;
		return;
	}

	for (i = 0; i < partdesc->nparts; i++)
	{
		Oid			childOID = partdesc->oids[i];
		Relation	childrel;

		/* Open rel, acquiring required locks */
		childrel = table_open(childOID, lockmode);

		/*
		 * Temporary partitions belonging to other sessions should have been
		 * disallowed at definition, but for paranoia's sake, let's double
		 * check.
		 */
		if (RELATION_IS_OTHER_TEMP(childrel))
			elog(ERROR, "temporary relation from another session found as partition");

		expand_single_inheritance_child(root, parentrte, parentRTindex,
										parentrel, top_parentrc, childrel,
										appinfos, &childrte, &childRTindex);

		/* If this child is itself partitioned, recurse */
		if (childrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			expand_partitioned_rtentry(root, childrte, childRTindex,
									   childrel, top_parentrc, lockmode,
									   appinfos);

		/* Close child relation, but keep locks */
		table_close(childrel, NoLock);
	}
}

/*
 * expand_single_inheritance_child
 *		Build a RangeTblEntry and an AppendRelInfo, if appropriate, plus
 *		maybe a PlanRowMark.
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
								List **appinfos, RangeTblEntry **childrte_p,
								Index *childRTindex_p)
{
	Query	   *parse = root->parse;
	Oid			parentOID = RelationGetRelid(parentrel);
	Oid			childOID = RelationGetRelid(childrel);
	RangeTblEntry *childrte;
	Index		childRTindex;
	AppendRelInfo *appinfo;

	/*
	 * Build an RTE for the child, and attach to query's rangetable list. We
	 * copy most fields of the parent's RTE, but replace relation OID and
	 * relkind, and set inh = false.  Also, set requiredPerms to zero since
	 * all required permissions checks are done on the original RTE. Likewise,
	 * set the child's securityQuals to empty, because we only want to apply
	 * the parent's RLS conditions regardless of what RLS properties
	 * individual children may have.  (This is an intentional choice to make
	 * inherited RLS work like regular permissions checks.) The parent
	 * securityQuals will be propagated to children along with other base
	 * restriction clauses, so we don't need to do it here.
	 */
	childrte = copyObject(parentrte);
	*childrte_p = childrte;
	childrte->relid = childOID;
	childrte->relkind = childrel->rd_rel->relkind;
	/* A partitioned child will need to be expanded further. */
	if (childOID != parentOID &&
		childrte->relkind == RELKIND_PARTITIONED_TABLE)
		childrte->inh = true;
	else
		childrte->inh = false;
	childrte->requiredPerms = 0;
	childrte->securityQuals = NIL;
	parse->rtable = lappend(parse->rtable, childrte);
	childRTindex = list_length(parse->rtable);
	*childRTindex_p = childRTindex;

	/*
	 * We need an AppendRelInfo if paths will be built for the child RTE. If
	 * childrte->inh is true, then we'll always need to generate append paths
	 * for it.  If childrte->inh is false, we must scan it if it's not a
	 * partitioned table; but if it is a partitioned table, then it never has
	 * any data of its own and need not be scanned.
	 */
	if (childrte->relkind != RELKIND_PARTITIONED_TABLE || childrte->inh)
	{
		appinfo = make_append_rel_info(parentrel, childrel,
									   parentRTindex, childRTindex);
		*appinfos = lappend(*appinfos, appinfo);

		/*
		 * Translate the column permissions bitmaps to the child's attnums (we
		 * have to build the translated_vars list before we can do this). But
		 * if this is the parent table, leave copyObject's result alone.
		 *
		 * Note: we need to do this even though the executor won't run any
		 * permissions checks on the child RTE.  The insertedCols/updatedCols
		 * bitmaps may be examined for trigger-firing purposes.
		 */
		if (childOID != parentOID)
		{
			childrte->selectedCols = translate_col_privs(parentrte->selectedCols,
														 appinfo->translated_vars);
			childrte->insertedCols = translate_col_privs(parentrte->insertedCols,
														 appinfo->translated_vars);
			childrte->updatedCols = translate_col_privs(parentrte->updatedCols,
														appinfo->translated_vars);
		}
	}

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
		 * the child tables be locked using appropriate mode).
		 */
		childrc->isParent = (childrte->relkind == RELKIND_PARTITIONED_TABLE);

		/* Include child's rowmark type in top parent's allMarkTypes */
		top_parentrc->allMarkTypes |= childrc->allMarkTypes;

		root->rowMarks = lappend(root->rowMarks, childrc);
	}
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
