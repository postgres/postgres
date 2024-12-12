/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/plancat.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "statistics/statistics.h"
#include "storage/bufmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* GUC parameter */
int			constraint_exclusion = CONSTRAINT_EXCLUSION_PARTITION;

/* Hook for plugins to get control in get_relation_info() */
get_relation_info_hook_type get_relation_info_hook = NULL;


static void get_relation_foreign_keys(PlannerInfo *root, RelOptInfo *rel,
									  Relation relation, bool inhparent);
static bool infer_collation_opclass_match(InferenceElem *elem, Relation idxRel,
										  List *idxExprs);
static List *get_relation_constraints(PlannerInfo *root,
									  Oid relationObjectId, RelOptInfo *rel,
									  bool include_noinherit,
									  bool include_notnull,
									  bool include_partition);
static List *build_index_tlist(PlannerInfo *root, IndexOptInfo *index,
							   Relation heapRelation);
static List *get_relation_statistics(RelOptInfo *rel, Relation relation);
static void set_relation_partition_info(PlannerInfo *root, RelOptInfo *rel,
										Relation relation);
static PartitionScheme find_partition_scheme(PlannerInfo *root,
											 Relation relation);
static void set_baserel_partition_key_exprs(Relation relation,
											RelOptInfo *rel);
static void set_baserel_partition_constraint(Relation relation,
											 RelOptInfo *rel);


/*
 * get_relation_info -
 *	  Retrieves catalog information for a given relation.
 *
 * Given the Oid of the relation, return the following info into fields
 * of the RelOptInfo struct:
 *
 *	min_attr	lowest valid AttrNumber
 *	max_attr	highest valid AttrNumber
 *	indexlist	list of IndexOptInfos for relation's indexes
 *	statlist	list of StatisticExtInfo for relation's statistic objects
 *	serverid	if it's a foreign table, the server OID
 *	fdwroutine	if it's a foreign table, the FDW function pointers
 *	pages		number of pages
 *	tuples		number of tuples
 *	rel_parallel_workers user-defined number of parallel workers
 *
 * Also, add information about the relation's foreign keys to root->fkey_list.
 *
 * Also, initialize the attr_needed[] and attr_widths[] arrays.  In most
 * cases these are left as zeroes, but sometimes we need to compute attr
 * widths here, and we may as well cache the results for costsize.c.
 *
 * If inhparent is true, all we need to do is set up the attr arrays:
 * the RelOptInfo actually represents the appendrel formed by an inheritance
 * tree, and so the parent rel's physical size and index information isn't
 * important for it, however, for partitioned tables, we do populate the
 * indexlist as the planner uses unique indexes as unique proofs for certain
 * optimizations.
 */
void
get_relation_info(PlannerInfo *root, Oid relationObjectId, bool inhparent,
				  RelOptInfo *rel)
{
	Index		varno = rel->relid;
	Relation	relation;
	bool		hasindex;
	List	   *indexinfos = NIL;

	/*
	 * We need not lock the relation since it was already locked, either by
	 * the rewriter or when expand_inherited_rtentry() added it to the query's
	 * rangetable.
	 */
	relation = table_open(relationObjectId, NoLock);

	/*
	 * Relations without a table AM can be used in a query only if they are of
	 * special-cased relkinds.  This check prevents us from crashing later if,
	 * for example, a view's ON SELECT rule has gone missing.  Note that
	 * table_open() already rejected indexes and composite types; spell the
	 * error the same way it does.
	 */
	if (!relation->rd_tableam)
	{
		if (!(relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE ||
			  relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot open relation \"%s\"",
							RelationGetRelationName(relation)),
					 errdetail_relkind_not_supported(relation->rd_rel->relkind)));
	}

	/* Temporary and unlogged relations are inaccessible during recovery. */
	if (!RelationIsPermanent(relation) && RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary or unlogged relations during recovery")));

	rel->min_attr = FirstLowInvalidHeapAttributeNumber + 1;
	rel->max_attr = RelationGetNumberOfAttributes(relation);
	rel->reltablespace = RelationGetForm(relation)->reltablespace;

	Assert(rel->max_attr >= rel->min_attr);
	rel->attr_needed = (Relids *)
		palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(Relids));
	rel->attr_widths = (int32 *)
		palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(int32));

	/*
	 * Record which columns are defined as NOT NULL.  We leave this
	 * unpopulated for non-partitioned inheritance parent relations as it's
	 * ambiguous as to what it means.  Some child tables may have a NOT NULL
	 * constraint for a column while others may not.  We could work harder and
	 * build a unioned set of all child relations notnullattnums, but there's
	 * currently no need.  The RelOptInfo corresponding to the !inh
	 * RangeTblEntry does get populated.
	 */
	if (!inhparent || relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		for (int i = 0; i < relation->rd_att->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(relation->rd_att, i);

			if (attr->attnotnull)
			{
				rel->notnullattnums = bms_add_member(rel->notnullattnums,
													 attr->attnum);

				/*
				 * Per RemoveAttributeById(), dropped columns will have their
				 * attnotnull unset, so we needn't check for dropped columns
				 * in the above condition.
				 */
				Assert(!attr->attisdropped);
			}
		}
	}

	/*
	 * Estimate relation size --- unless it's an inheritance parent, in which
	 * case the size we want is not the rel's own size but the size of its
	 * inheritance tree.  That will be computed in set_append_rel_size().
	 */
	if (!inhparent)
		estimate_rel_size(relation, rel->attr_widths - rel->min_attr,
						  &rel->pages, &rel->tuples, &rel->allvisfrac);

	/* Retrieve the parallel_workers reloption, or -1 if not set. */
	rel->rel_parallel_workers = RelationGetParallelWorkers(relation, -1);

	/*
	 * Make list of indexes.  Ignore indexes on system catalogs if told to.
	 * Don't bother with indexes from traditional inheritance parents.  For
	 * partitioned tables, we need a list of at least unique indexes as these
	 * serve as unique proofs for certain planner optimizations.  However,
	 * let's not discriminate here and just record all partitioned indexes
	 * whether they're unique indexes or not.
	 */
	if ((inhparent && relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		|| (IgnoreSystemIndexes && IsSystemRelation(relation)))
		hasindex = false;
	else
		hasindex = relation->rd_rel->relhasindex;

	if (hasindex)
	{
		List	   *indexoidlist;
		LOCKMODE	lmode;
		ListCell   *l;

		indexoidlist = RelationGetIndexList(relation);

		/*
		 * For each index, we get the same type of lock that the executor will
		 * need, and do not release it.  This saves a couple of trips to the
		 * shared lock manager while not creating any real loss of
		 * concurrency, because no schema changes could be happening on the
		 * index while we hold lock on the parent rel, and no lock type used
		 * for queries blocks any other kind of index operation.
		 */
		lmode = root->simple_rte_array[varno]->rellockmode;

		foreach(l, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(l);
			Relation	indexRelation;
			Form_pg_index index;
			IndexAmRoutine *amroutine = NULL;
			IndexOptInfo *info;
			int			ncolumns,
						nkeycolumns;
			int			i;

			/*
			 * Extract info from the relation descriptor for the index.
			 */
			indexRelation = index_open(indexoid, lmode);
			index = indexRelation->rd_index;

			/*
			 * Ignore invalid indexes, since they can't safely be used for
			 * queries.  Note that this is OK because the data structure we
			 * are constructing is only used by the planner --- the executor
			 * still needs to insert into "invalid" indexes, if they're marked
			 * indisready.
			 */
			if (!index->indisvalid)
			{
				index_close(indexRelation, NoLock);
				continue;
			}

			/*
			 * If the index is valid, but cannot yet be used, ignore it; but
			 * mark the plan we are generating as transient. See
			 * src/backend/access/heap/README.HOT for discussion.
			 */
			if (index->indcheckxmin &&
				!TransactionIdPrecedes(HeapTupleHeaderGetXmin(indexRelation->rd_indextuple->t_data),
									   TransactionXmin))
			{
				root->glob->transientPlan = true;
				index_close(indexRelation, NoLock);
				continue;
			}

			info = makeNode(IndexOptInfo);

			info->indexoid = index->indexrelid;
			info->reltablespace =
				RelationGetForm(indexRelation)->reltablespace;
			info->rel = rel;
			info->ncolumns = ncolumns = index->indnatts;
			info->nkeycolumns = nkeycolumns = index->indnkeyatts;

			info->indexkeys = (int *) palloc(sizeof(int) * ncolumns);
			info->indexcollations = (Oid *) palloc(sizeof(Oid) * nkeycolumns);
			info->opfamily = (Oid *) palloc(sizeof(Oid) * nkeycolumns);
			info->opcintype = (Oid *) palloc(sizeof(Oid) * nkeycolumns);
			info->canreturn = (bool *) palloc(sizeof(bool) * ncolumns);

			for (i = 0; i < ncolumns; i++)
			{
				info->indexkeys[i] = index->indkey.values[i];
				info->canreturn[i] = index_can_return(indexRelation, i + 1);
			}

			for (i = 0; i < nkeycolumns; i++)
			{
				info->opfamily[i] = indexRelation->rd_opfamily[i];
				info->opcintype[i] = indexRelation->rd_opcintype[i];
				info->indexcollations[i] = indexRelation->rd_indcollation[i];
			}

			info->relam = indexRelation->rd_rel->relam;

			/*
			 * We don't have an AM for partitioned indexes, so we'll just
			 * NULLify the AM related fields for those.
			 */
			if (indexRelation->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
			{
				/* We copy just the fields we need, not all of rd_indam */
				amroutine = indexRelation->rd_indam;
				info->amcanorderbyop = amroutine->amcanorderbyop;
				info->amoptionalkey = amroutine->amoptionalkey;
				info->amsearcharray = amroutine->amsearcharray;
				info->amsearchnulls = amroutine->amsearchnulls;
				info->amcanparallel = amroutine->amcanparallel;
				info->amhasgettuple = (amroutine->amgettuple != NULL);
				info->amhasgetbitmap = amroutine->amgetbitmap != NULL &&
					relation->rd_tableam->scan_bitmap_next_block != NULL;
				info->amcanmarkpos = (amroutine->ammarkpos != NULL &&
									  amroutine->amrestrpos != NULL);
				info->amcostestimate = amroutine->amcostestimate;
				Assert(info->amcostestimate != NULL);

				/* Fetch index opclass options */
				info->opclassoptions = RelationGetIndexAttOptions(indexRelation, true);

				/*
				 * Fetch the ordering information for the index, if any.
				 */
				if (info->relam == BTREE_AM_OID)
				{
					/*
					 * If it's a btree index, we can use its opfamily OIDs
					 * directly as the sort ordering opfamily OIDs.
					 */
					Assert(amroutine->amcanorder);

					info->sortopfamily = info->opfamily;
					info->reverse_sort = (bool *) palloc(sizeof(bool) * nkeycolumns);
					info->nulls_first = (bool *) palloc(sizeof(bool) * nkeycolumns);

					for (i = 0; i < nkeycolumns; i++)
					{
						int16		opt = indexRelation->rd_indoption[i];

						info->reverse_sort[i] = (opt & INDOPTION_DESC) != 0;
						info->nulls_first[i] = (opt & INDOPTION_NULLS_FIRST) != 0;
					}
				}
				else if (amroutine->amcanorder)
				{
					/*
					 * Otherwise, identify the corresponding btree opfamilies
					 * by trying to map this index's "<" operators into btree.
					 * Since "<" uniquely defines the behavior of a sort
					 * order, this is a sufficient test.
					 *
					 * XXX This method is rather slow and also requires the
					 * undesirable assumption that the other index AM numbers
					 * its strategies the same as btree.  It'd be better to
					 * have a way to explicitly declare the corresponding
					 * btree opfamily for each opfamily of the other index
					 * type.  But given the lack of current or foreseeable
					 * amcanorder index types, it's not worth expending more
					 * effort on now.
					 */
					info->sortopfamily = (Oid *) palloc(sizeof(Oid) * nkeycolumns);
					info->reverse_sort = (bool *) palloc(sizeof(bool) * nkeycolumns);
					info->nulls_first = (bool *) palloc(sizeof(bool) * nkeycolumns);

					for (i = 0; i < nkeycolumns; i++)
					{
						int16		opt = indexRelation->rd_indoption[i];
						Oid			ltopr;
						Oid			btopfamily;
						Oid			btopcintype;
						int16		btstrategy;

						info->reverse_sort[i] = (opt & INDOPTION_DESC) != 0;
						info->nulls_first[i] = (opt & INDOPTION_NULLS_FIRST) != 0;

						ltopr = get_opfamily_member(info->opfamily[i],
													info->opcintype[i],
													info->opcintype[i],
													BTLessStrategyNumber);
						if (OidIsValid(ltopr) &&
							get_ordering_op_properties(ltopr,
													   &btopfamily,
													   &btopcintype,
													   &btstrategy) &&
							btopcintype == info->opcintype[i] &&
							btstrategy == BTLessStrategyNumber)
						{
							/* Successful mapping */
							info->sortopfamily[i] = btopfamily;
						}
						else
						{
							/* Fail ... quietly treat index as unordered */
							info->sortopfamily = NULL;
							info->reverse_sort = NULL;
							info->nulls_first = NULL;
							break;
						}
					}
				}
				else
				{
					info->sortopfamily = NULL;
					info->reverse_sort = NULL;
					info->nulls_first = NULL;
				}
			}
			else
			{
				info->amcanorderbyop = false;
				info->amoptionalkey = false;
				info->amsearcharray = false;
				info->amsearchnulls = false;
				info->amcanparallel = false;
				info->amhasgettuple = false;
				info->amhasgetbitmap = false;
				info->amcanmarkpos = false;
				info->amcostestimate = NULL;

				info->sortopfamily = NULL;
				info->reverse_sort = NULL;
				info->nulls_first = NULL;
			}

			/*
			 * Fetch the index expressions and predicate, if any.  We must
			 * modify the copies we obtain from the relcache to have the
			 * correct varno for the parent relation, so that they match up
			 * correctly against qual clauses.
			 */
			info->indexprs = RelationGetIndexExpressions(indexRelation);
			info->indpred = RelationGetIndexPredicate(indexRelation);
			if (info->indexprs && varno != 1)
				ChangeVarNodes((Node *) info->indexprs, 1, varno, 0);
			if (info->indpred && varno != 1)
				ChangeVarNodes((Node *) info->indpred, 1, varno, 0);

			/* Build targetlist using the completed indexprs data */
			info->indextlist = build_index_tlist(root, info, relation);

			info->indrestrictinfo = NIL;	/* set later, in indxpath.c */
			info->predOK = false;	/* set later, in indxpath.c */
			info->unique = index->indisunique;
			info->nullsnotdistinct = index->indnullsnotdistinct;
			info->immediate = index->indimmediate;
			info->hypothetical = false;

			/*
			 * Estimate the index size.  If it's not a partial index, we lock
			 * the number-of-tuples estimate to equal the parent table; if it
			 * is partial then we have to use the same methods as we would for
			 * a table, except we can be sure that the index is not larger
			 * than the table.  We must ignore partitioned indexes here as
			 * there are not physical indexes.
			 */
			if (indexRelation->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
			{
				if (info->indpred == NIL)
				{
					info->pages = RelationGetNumberOfBlocks(indexRelation);
					info->tuples = rel->tuples;
				}
				else
				{
					double		allvisfrac; /* dummy */

					estimate_rel_size(indexRelation, NULL,
									  &info->pages, &info->tuples, &allvisfrac);
					if (info->tuples > rel->tuples)
						info->tuples = rel->tuples;
				}

				/*
				 * Get tree height while we have the index open
				 */
				if (amroutine->amgettreeheight)
				{
					info->tree_height = amroutine->amgettreeheight(indexRelation);
				}
				else
				{
					/* For other index types, just set it to "unknown" for now */
					info->tree_height = -1;
				}
			}
			else
			{
				/* Zero these out for partitioned indexes */
				info->pages = 0;
				info->tuples = 0.0;
				info->tree_height = -1;
			}

			index_close(indexRelation, NoLock);

			/*
			 * We've historically used lcons() here.  It'd make more sense to
			 * use lappend(), but that causes the planner to change behavior
			 * in cases where two indexes seem equally attractive.  For now,
			 * stick with lcons() --- few tables should have so many indexes
			 * that the O(N^2) behavior of lcons() is really a problem.
			 */
			indexinfos = lcons(info, indexinfos);
		}

		list_free(indexoidlist);
	}

	rel->indexlist = indexinfos;

	rel->statlist = get_relation_statistics(rel, relation);

	/* Grab foreign-table info using the relcache, while we have it */
	if (relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		/* Check if the access to foreign tables is restricted */
		if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_FOREIGN_TABLE) != 0))
		{
			/* there must not be built-in foreign tables */
			Assert(RelationGetRelid(relation) >= FirstNormalObjectId);

			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("access to non-system foreign table is restricted")));
		}

		rel->serverid = GetForeignServerIdByRelId(RelationGetRelid(relation));
		rel->fdwroutine = GetFdwRoutineForRelation(relation, true);
	}
	else
	{
		rel->serverid = InvalidOid;
		rel->fdwroutine = NULL;
	}

	/* Collect info about relation's foreign keys, if relevant */
	get_relation_foreign_keys(root, rel, relation, inhparent);

	/* Collect info about functions implemented by the rel's table AM. */
	if (relation->rd_tableam &&
		relation->rd_tableam->scan_set_tidrange != NULL &&
		relation->rd_tableam->scan_getnextslot_tidrange != NULL)
		rel->amflags |= AMFLAG_HAS_TID_RANGE;

	/*
	 * Collect info about relation's partitioning scheme, if any. Only
	 * inheritance parents may be partitioned.
	 */
	if (inhparent && relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		set_relation_partition_info(root, rel, relation);

	table_close(relation, NoLock);

	/*
	 * Allow a plugin to editorialize on the info we obtained from the
	 * catalogs.  Actions might include altering the assumed relation size,
	 * removing an index, or adding a hypothetical index to the indexlist.
	 */
	if (get_relation_info_hook)
		(*get_relation_info_hook) (root, relationObjectId, inhparent, rel);
}

/*
 * get_relation_foreign_keys -
 *	  Retrieves foreign key information for a given relation.
 *
 * ForeignKeyOptInfos for relevant foreign keys are created and added to
 * root->fkey_list.  We do this now while we have the relcache entry open.
 * We could sometimes avoid making useless ForeignKeyOptInfos if we waited
 * until all RelOptInfos have been built, but the cost of re-opening the
 * relcache entries would probably exceed any savings.
 */
static void
get_relation_foreign_keys(PlannerInfo *root, RelOptInfo *rel,
						  Relation relation, bool inhparent)
{
	List	   *rtable = root->parse->rtable;
	List	   *cachedfkeys;
	ListCell   *lc;

	/*
	 * If it's not a baserel, we don't care about its FKs.  Also, if the query
	 * references only a single relation, we can skip the lookup since no FKs
	 * could satisfy the requirements below.
	 */
	if (rel->reloptkind != RELOPT_BASEREL ||
		list_length(rtable) < 2)
		return;

	/*
	 * If it's the parent of an inheritance tree, ignore its FKs.  We could
	 * make useful FK-based deductions if we found that all members of the
	 * inheritance tree have equivalent FK constraints, but detecting that
	 * would require code that hasn't been written.
	 */
	if (inhparent)
		return;

	/*
	 * Extract data about relation's FKs from the relcache.  Note that this
	 * list belongs to the relcache and might disappear in a cache flush, so
	 * we must not do any further catalog access within this function.
	 */
	cachedfkeys = RelationGetFKeyList(relation);

	/*
	 * Figure out which FKs are of interest for this query, and create
	 * ForeignKeyOptInfos for them.  We want only FKs that reference some
	 * other RTE of the current query.  In queries containing self-joins,
	 * there might be more than one other RTE for a referenced table, and we
	 * should make a ForeignKeyOptInfo for each occurrence.
	 *
	 * Ideally, we would ignore RTEs that correspond to non-baserels, but it's
	 * too hard to identify those here, so we might end up making some useless
	 * ForeignKeyOptInfos.  If so, match_foreign_keys_to_quals() will remove
	 * them again.
	 */
	foreach(lc, cachedfkeys)
	{
		ForeignKeyCacheInfo *cachedfk = (ForeignKeyCacheInfo *) lfirst(lc);
		Index		rti;
		ListCell   *lc2;

		/* conrelid should always be that of the table we're considering */
		Assert(cachedfk->conrelid == RelationGetRelid(relation));

		/* Scan to find other RTEs matching confrelid */
		rti = 0;
		foreach(lc2, rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc2);
			ForeignKeyOptInfo *info;

			rti++;
			/* Ignore if not the correct table */
			if (rte->rtekind != RTE_RELATION ||
				rte->relid != cachedfk->confrelid)
				continue;
			/* Ignore if it's an inheritance parent; doesn't really match */
			if (rte->inh)
				continue;
			/* Ignore self-referential FKs; we only care about joins */
			if (rti == rel->relid)
				continue;

			/* OK, let's make an entry */
			info = makeNode(ForeignKeyOptInfo);
			info->con_relid = rel->relid;
			info->ref_relid = rti;
			info->nkeys = cachedfk->nkeys;
			memcpy(info->conkey, cachedfk->conkey, sizeof(info->conkey));
			memcpy(info->confkey, cachedfk->confkey, sizeof(info->confkey));
			memcpy(info->conpfeqop, cachedfk->conpfeqop, sizeof(info->conpfeqop));
			/* zero out fields to be filled by match_foreign_keys_to_quals */
			info->nmatched_ec = 0;
			info->nconst_ec = 0;
			info->nmatched_rcols = 0;
			info->nmatched_ri = 0;
			memset(info->eclass, 0, sizeof(info->eclass));
			memset(info->fk_eclass_member, 0, sizeof(info->fk_eclass_member));
			memset(info->rinfos, 0, sizeof(info->rinfos));

			root->fkey_list = lappend(root->fkey_list, info);
		}
	}
}

/*
 * infer_arbiter_indexes -
 *	  Determine the unique indexes used to arbitrate speculative insertion.
 *
 * Uses user-supplied inference clause expressions and predicate to match a
 * unique index from those defined and ready on the heap relation (target).
 * An exact match is required on columns/expressions (although they can appear
 * in any order).  However, the predicate given by the user need only restrict
 * insertion to a subset of some part of the table covered by some particular
 * unique index (in particular, a partial unique index) in order to be
 * inferred.
 *
 * The implementation does not consider which B-Tree operator class any
 * particular available unique index attribute uses, unless one was specified
 * in the inference specification. The same is true of collations.  In
 * particular, there is no system dependency on the default operator class for
 * the purposes of inference.  If no opclass (or collation) is specified, then
 * all matching indexes (that may or may not match the default in terms of
 * each attribute opclass/collation) are used for inference.
 */
List *
infer_arbiter_indexes(PlannerInfo *root)
{
	OnConflictExpr *onconflict = root->parse->onConflict;

	/* Iteration state */
	Index		varno;
	RangeTblEntry *rte;
	Relation	relation;
	Oid			indexOidFromConstraint = InvalidOid;
	List	   *indexList;
	ListCell   *l;

	/* Normalized inference attributes and inference expressions: */
	Bitmapset  *inferAttrs = NULL;
	List	   *inferElems = NIL;

	/* Results */
	List	   *results = NIL;

	/*
	 * Quickly return NIL for ON CONFLICT DO NOTHING without an inference
	 * specification or named constraint.  ON CONFLICT DO UPDATE statements
	 * must always provide one or the other (but parser ought to have caught
	 * that already).
	 */
	if (onconflict->arbiterElems == NIL &&
		onconflict->constraint == InvalidOid)
		return NIL;

	/*
	 * We need not lock the relation since it was already locked, either by
	 * the rewriter or when expand_inherited_rtentry() added it to the query's
	 * rangetable.
	 */
	varno = root->parse->resultRelation;
	rte = rt_fetch(varno, root->parse->rtable);

	relation = table_open(rte->relid, NoLock);

	/*
	 * Build normalized/BMS representation of plain indexed attributes, as
	 * well as a separate list of expression items.  This simplifies matching
	 * the cataloged definition of indexes.
	 */
	foreach(l, onconflict->arbiterElems)
	{
		InferenceElem *elem = (InferenceElem *) lfirst(l);
		Var		   *var;
		int			attno;

		if (!IsA(elem->expr, Var))
		{
			/* If not a plain Var, just shove it in inferElems for now */
			inferElems = lappend(inferElems, elem->expr);
			continue;
		}

		var = (Var *) elem->expr;
		attno = var->varattno;

		if (attno == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("whole row unique index inference specifications are not supported")));

		inferAttrs = bms_add_member(inferAttrs,
									attno - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * Lookup named constraint's index.  This is not immediately returned
	 * because some additional sanity checks are required.
	 */
	if (onconflict->constraint != InvalidOid)
	{
		indexOidFromConstraint = get_constraint_index(onconflict->constraint);

		if (indexOidFromConstraint == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("constraint in ON CONFLICT clause has no associated index")));
	}

	/*
	 * Using that representation, iterate through the list of indexes on the
	 * target relation to try and find a match
	 */
	indexList = RelationGetIndexList(relation);

	foreach(l, indexList)
	{
		Oid			indexoid = lfirst_oid(l);
		Relation	idxRel;
		Form_pg_index idxForm;
		Bitmapset  *indexedAttrs;
		List	   *idxExprs;
		List	   *predExprs;
		AttrNumber	natt;
		ListCell   *el;

		/*
		 * Extract info from the relation descriptor for the index.  Obtain
		 * the same lock type that the executor will ultimately use.
		 *
		 * Let executor complain about !indimmediate case directly, because
		 * enforcement needs to occur there anyway when an inference clause is
		 * omitted.
		 */
		idxRel = index_open(indexoid, rte->rellockmode);
		idxForm = idxRel->rd_index;

		if (!idxForm->indisvalid)
			goto next;

		/*
		 * Note that we do not perform a check against indcheckxmin (like e.g.
		 * get_relation_info()) here to eliminate candidates, because
		 * uniqueness checking only cares about the most recently committed
		 * tuple versions.
		 */

		/*
		 * Look for match on "ON constraint_name" variant, which may not be
		 * unique constraint.  This can only be a constraint name.
		 */
		if (indexOidFromConstraint == idxForm->indexrelid)
		{
			if (idxForm->indisexclusion && onconflict->action == ONCONFLICT_UPDATE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("ON CONFLICT DO UPDATE not supported with exclusion constraints")));

			results = lappend_oid(results, idxForm->indexrelid);
			list_free(indexList);
			index_close(idxRel, NoLock);
			table_close(relation, NoLock);
			return results;
		}
		else if (indexOidFromConstraint != InvalidOid)
		{
			/* No point in further work for index in named constraint case */
			goto next;
		}

		/*
		 * Only considering conventional inference at this point (not named
		 * constraints), so index under consideration can be immediately
		 * skipped if it's not unique
		 */
		if (!idxForm->indisunique)
			goto next;

		/*
		 * So-called unique constraints with WITHOUT OVERLAPS are really
		 * exclusion constraints, so skip those too.
		 */
		if (idxForm->indisexclusion)
			goto next;

		/* Build BMS representation of plain (non expression) index attrs */
		indexedAttrs = NULL;
		for (natt = 0; natt < idxForm->indnkeyatts; natt++)
		{
			int			attno = idxRel->rd_index->indkey.values[natt];

			if (attno != 0)
				indexedAttrs = bms_add_member(indexedAttrs,
											  attno - FirstLowInvalidHeapAttributeNumber);
		}

		/* Non-expression attributes (if any) must match */
		if (!bms_equal(indexedAttrs, inferAttrs))
			goto next;

		/* Expression attributes (if any) must match */
		idxExprs = RelationGetIndexExpressions(idxRel);
		if (idxExprs && varno != 1)
			ChangeVarNodes((Node *) idxExprs, 1, varno, 0);

		foreach(el, onconflict->arbiterElems)
		{
			InferenceElem *elem = (InferenceElem *) lfirst(el);

			/*
			 * Ensure that collation/opclass aspects of inference expression
			 * element match.  Even though this loop is primarily concerned
			 * with matching expressions, it is a convenient point to check
			 * this for both expressions and ordinary (non-expression)
			 * attributes appearing as inference elements.
			 */
			if (!infer_collation_opclass_match(elem, idxRel, idxExprs))
				goto next;

			/*
			 * Plain Vars don't factor into count of expression elements, and
			 * the question of whether or not they satisfy the index
			 * definition has already been considered (they must).
			 */
			if (IsA(elem->expr, Var))
				continue;

			/*
			 * Might as well avoid redundant check in the rare cases where
			 * infer_collation_opclass_match() is required to do real work.
			 * Otherwise, check that element expression appears in cataloged
			 * index definition.
			 */
			if (elem->infercollid != InvalidOid ||
				elem->inferopclass != InvalidOid ||
				list_member(idxExprs, elem->expr))
				continue;

			goto next;
		}

		/*
		 * Now that all inference elements were matched, ensure that the
		 * expression elements from inference clause are not missing any
		 * cataloged expressions.  This does the right thing when unique
		 * indexes redundantly repeat the same attribute, or if attributes
		 * redundantly appear multiple times within an inference clause.
		 */
		if (list_difference(idxExprs, inferElems) != NIL)
			goto next;

		/*
		 * If it's a partial index, its predicate must be implied by the ON
		 * CONFLICT's WHERE clause.
		 */
		predExprs = RelationGetIndexPredicate(idxRel);
		if (predExprs && varno != 1)
			ChangeVarNodes((Node *) predExprs, 1, varno, 0);

		if (!predicate_implied_by(predExprs, (List *) onconflict->arbiterWhere, false))
			goto next;

		results = lappend_oid(results, idxForm->indexrelid);
next:
		index_close(idxRel, NoLock);
	}

	list_free(indexList);
	table_close(relation, NoLock);

	if (results == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("there is no unique or exclusion constraint matching the ON CONFLICT specification")));

	return results;
}

/*
 * infer_collation_opclass_match - ensure infer element opclass/collation match
 *
 * Given unique index inference element from inference specification, if
 * collation was specified, or if opclass was specified, verify that there is
 * at least one matching indexed attribute (occasionally, there may be more).
 * Skip this in the common case where inference specification does not include
 * collation or opclass (instead matching everything, regardless of cataloged
 * collation/opclass of indexed attribute).
 *
 * At least historically, Postgres has not offered collations or opclasses
 * with alternative-to-default notions of equality, so these additional
 * criteria should only be required infrequently.
 *
 * Don't give up immediately when an inference element matches some attribute
 * cataloged as indexed but not matching additional opclass/collation
 * criteria.  This is done so that the implementation is as forgiving as
 * possible of redundancy within cataloged index attributes (or, less
 * usefully, within inference specification elements).  If collations actually
 * differ between apparently redundantly indexed attributes (redundant within
 * or across indexes), then there really is no redundancy as such.
 *
 * Note that if an inference element specifies an opclass and a collation at
 * once, both must match in at least one particular attribute within index
 * catalog definition in order for that inference element to be considered
 * inferred/satisfied.
 */
static bool
infer_collation_opclass_match(InferenceElem *elem, Relation idxRel,
							  List *idxExprs)
{
	AttrNumber	natt;
	Oid			inferopfamily = InvalidOid; /* OID of opclass opfamily */
	Oid			inferopcinputtype = InvalidOid; /* OID of opclass input type */
	int			nplain = 0;		/* # plain attrs observed */

	/*
	 * If inference specification element lacks collation/opclass, then no
	 * need to check for exact match.
	 */
	if (elem->infercollid == InvalidOid && elem->inferopclass == InvalidOid)
		return true;

	/*
	 * Lookup opfamily and input type, for matching indexes
	 */
	if (elem->inferopclass)
	{
		inferopfamily = get_opclass_family(elem->inferopclass);
		inferopcinputtype = get_opclass_input_type(elem->inferopclass);
	}

	for (natt = 1; natt <= idxRel->rd_att->natts; natt++)
	{
		Oid			opfamily = idxRel->rd_opfamily[natt - 1];
		Oid			opcinputtype = idxRel->rd_opcintype[natt - 1];
		Oid			collation = idxRel->rd_indcollation[natt - 1];
		int			attno = idxRel->rd_index->indkey.values[natt - 1];

		if (attno != 0)
			nplain++;

		if (elem->inferopclass != InvalidOid &&
			(inferopfamily != opfamily || inferopcinputtype != opcinputtype))
		{
			/* Attribute needed to match opclass, but didn't */
			continue;
		}

		if (elem->infercollid != InvalidOid &&
			elem->infercollid != collation)
		{
			/* Attribute needed to match collation, but didn't */
			continue;
		}

		/* If one matching index att found, good enough -- return true */
		if (IsA(elem->expr, Var))
		{
			if (((Var *) elem->expr)->varattno == attno)
				return true;
		}
		else if (attno == 0)
		{
			Node	   *nattExpr = list_nth(idxExprs, (natt - 1) - nplain);

			/*
			 * Note that unlike routines like match_index_to_operand() we
			 * don't need to care about RelabelType.  Neither the index
			 * definition nor the inference clause should contain them.
			 */
			if (equal(elem->expr, nattExpr))
				return true;
		}
	}

	return false;
}

/*
 * estimate_rel_size - estimate # pages and # tuples in a table or index
 *
 * We also estimate the fraction of the pages that are marked all-visible in
 * the visibility map, for use in estimation of index-only scans.
 *
 * If attr_widths isn't NULL, it points to the zero-index entry of the
 * relation's attr_widths[] cache; we fill this in if we have need to compute
 * the attribute widths for estimation purposes.
 */
void
estimate_rel_size(Relation rel, int32 *attr_widths,
				  BlockNumber *pages, double *tuples, double *allvisfrac)
{
	BlockNumber curpages;
	BlockNumber relpages;
	double		reltuples;
	BlockNumber relallvisible;
	double		density;

	if (RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
	{
		table_relation_estimate_size(rel, attr_widths, pages, tuples,
									 allvisfrac);
	}
	else if (rel->rd_rel->relkind == RELKIND_INDEX)
	{
		/*
		 * XXX: It'd probably be good to move this into a callback, individual
		 * index types e.g. know if they have a metapage.
		 */

		/* it has storage, ok to call the smgr */
		curpages = RelationGetNumberOfBlocks(rel);

		/* report estimated # pages */
		*pages = curpages;
		/* quick exit if rel is clearly empty */
		if (curpages == 0)
		{
			*tuples = 0;
			*allvisfrac = 0;
			return;
		}

		/* coerce values in pg_class to more desirable types */
		relpages = (BlockNumber) rel->rd_rel->relpages;
		reltuples = (double) rel->rd_rel->reltuples;
		relallvisible = (BlockNumber) rel->rd_rel->relallvisible;

		/*
		 * Discount the metapage while estimating the number of tuples. This
		 * is a kluge because it assumes more than it ought to about index
		 * structure.  Currently it's OK for btree, hash, and GIN indexes but
		 * suspect for GiST indexes.
		 */
		if (relpages > 0)
		{
			curpages--;
			relpages--;
		}

		/* estimate number of tuples from previous tuple density */
		if (reltuples >= 0 && relpages > 0)
			density = reltuples / (double) relpages;
		else
		{
			/*
			 * If we have no data because the relation was never vacuumed,
			 * estimate tuple width from attribute datatypes.  We assume here
			 * that the pages are completely full, which is OK for tables
			 * (since they've presumably not been VACUUMed yet) but is
			 * probably an overestimate for indexes.  Fortunately
			 * get_relation_info() can clamp the overestimate to the parent
			 * table's size.
			 *
			 * Note: this code intentionally disregards alignment
			 * considerations, because (a) that would be gilding the lily
			 * considering how crude the estimate is, and (b) it creates
			 * platform dependencies in the default plans which are kind of a
			 * headache for regression testing.
			 *
			 * XXX: Should this logic be more index specific?
			 */
			int32		tuple_width;

			tuple_width = get_rel_data_width(rel, attr_widths);
			tuple_width += MAXALIGN(SizeofHeapTupleHeader);
			tuple_width += sizeof(ItemIdData);
			/* note: integer division is intentional here */
			density = (BLCKSZ - SizeOfPageHeaderData) / tuple_width;
		}
		*tuples = rint(density * (double) curpages);

		/*
		 * We use relallvisible as-is, rather than scaling it up like we do
		 * for the pages and tuples counts, on the theory that any pages added
		 * since the last VACUUM are most likely not marked all-visible.  But
		 * costsize.c wants it converted to a fraction.
		 */
		if (relallvisible == 0 || curpages <= 0)
			*allvisfrac = 0;
		else if ((double) relallvisible >= curpages)
			*allvisfrac = 1;
		else
			*allvisfrac = (double) relallvisible / curpages;
	}
	else
	{
		/*
		 * Just use whatever's in pg_class.  This covers foreign tables,
		 * sequences, and also relkinds without storage (shouldn't get here?);
		 * see initializations in AddNewRelationTuple().  Note that FDW must
		 * cope if reltuples is -1!
		 */
		*pages = rel->rd_rel->relpages;
		*tuples = rel->rd_rel->reltuples;
		*allvisfrac = 0;
	}
}


/*
 * get_rel_data_width
 *
 * Estimate the average width of (the data part of) the relation's tuples.
 *
 * If attr_widths isn't NULL, it points to the zero-index entry of the
 * relation's attr_widths[] cache; use and update that cache as appropriate.
 *
 * Currently we ignore dropped columns.  Ideally those should be included
 * in the result, but we haven't got any way to get info about them; and
 * since they might be mostly NULLs, treating them as zero-width is not
 * necessarily the wrong thing anyway.
 */
int32
get_rel_data_width(Relation rel, int32 *attr_widths)
{
	int64		tuple_width = 0;
	int			i;

	for (i = 1; i <= RelationGetNumberOfAttributes(rel); i++)
	{
		Form_pg_attribute att = TupleDescAttr(rel->rd_att, i - 1);
		int32		item_width;

		if (att->attisdropped)
			continue;

		/* use previously cached data, if any */
		if (attr_widths != NULL && attr_widths[i] > 0)
		{
			tuple_width += attr_widths[i];
			continue;
		}

		/* This should match set_rel_width() in costsize.c */
		item_width = get_attavgwidth(RelationGetRelid(rel), i);
		if (item_width <= 0)
		{
			item_width = get_typavgwidth(att->atttypid, att->atttypmod);
			Assert(item_width > 0);
		}
		if (attr_widths != NULL)
			attr_widths[i] = item_width;
		tuple_width += item_width;
	}

	return clamp_width_est(tuple_width);
}

/*
 * get_relation_data_width
 *
 * External API for get_rel_data_width: same behavior except we have to
 * open the relcache entry.
 */
int32
get_relation_data_width(Oid relid, int32 *attr_widths)
{
	int32		result;
	Relation	relation;

	/* As above, assume relation is already locked */
	relation = table_open(relid, NoLock);

	result = get_rel_data_width(relation, attr_widths);

	table_close(relation, NoLock);

	return result;
}


/*
 * get_relation_constraints
 *
 * Retrieve the applicable constraint expressions of the given relation.
 *
 * Returns a List (possibly empty) of constraint expressions.  Each one
 * has been canonicalized, and its Vars are changed to have the varno
 * indicated by rel->relid.  This allows the expressions to be easily
 * compared to expressions taken from WHERE.
 *
 * If include_noinherit is true, it's okay to include constraints that
 * are marked NO INHERIT.
 *
 * If include_notnull is true, "col IS NOT NULL" expressions are generated
 * and added to the result for each column that's marked attnotnull.
 *
 * If include_partition is true, and the relation is a partition,
 * also include the partitioning constraints.
 *
 * Note: at present this is invoked at most once per relation per planner
 * run, and in many cases it won't be invoked at all, so there seems no
 * point in caching the data in RelOptInfo.
 */
static List *
get_relation_constraints(PlannerInfo *root,
						 Oid relationObjectId, RelOptInfo *rel,
						 bool include_noinherit,
						 bool include_notnull,
						 bool include_partition)
{
	List	   *result = NIL;
	Index		varno = rel->relid;
	Relation	relation;
	TupleConstr *constr;

	/*
	 * We assume the relation has already been safely locked.
	 */
	relation = table_open(relationObjectId, NoLock);

	constr = relation->rd_att->constr;
	if (constr != NULL)
	{
		int			num_check = constr->num_check;
		int			i;

		for (i = 0; i < num_check; i++)
		{
			Node	   *cexpr;

			/*
			 * If this constraint hasn't been fully validated yet, we must
			 * ignore it here.  Also ignore if NO INHERIT and we weren't told
			 * that that's safe.
			 */
			if (!constr->check[i].ccvalid)
				continue;
			if (constr->check[i].ccnoinherit && !include_noinherit)
				continue;

			cexpr = stringToNode(constr->check[i].ccbin);

			/*
			 * Run each expression through const-simplification and
			 * canonicalization.  This is not just an optimization, but is
			 * necessary, because we will be comparing it to
			 * similarly-processed qual clauses, and may fail to detect valid
			 * matches without this.  This must match the processing done to
			 * qual clauses in preprocess_expression()!  (We can skip the
			 * stuff involving subqueries, however, since we don't allow any
			 * in check constraints.)
			 */
			cexpr = eval_const_expressions(root, cexpr);

			cexpr = (Node *) canonicalize_qual((Expr *) cexpr, true);

			/* Fix Vars to have the desired varno */
			if (varno != 1)
				ChangeVarNodes(cexpr, 1, varno, 0);

			/*
			 * Finally, convert to implicit-AND format (that is, a List) and
			 * append the resulting item(s) to our output list.
			 */
			result = list_concat(result,
								 make_ands_implicit((Expr *) cexpr));
		}

		/* Add NOT NULL constraints in expression form, if requested */
		if (include_notnull && constr->has_not_null)
		{
			int			natts = relation->rd_att->natts;

			for (i = 1; i <= natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(relation->rd_att, i - 1);

				if (att->attnotnull && !att->attisdropped)
				{
					NullTest   *ntest = makeNode(NullTest);

					ntest->arg = (Expr *) makeVar(varno,
												  i,
												  att->atttypid,
												  att->atttypmod,
												  att->attcollation,
												  0);
					ntest->nulltesttype = IS_NOT_NULL;

					/*
					 * argisrow=false is correct even for a composite column,
					 * because attnotnull does not represent a SQL-spec IS NOT
					 * NULL test in such a case, just IS DISTINCT FROM NULL.
					 */
					ntest->argisrow = false;
					ntest->location = -1;
					result = lappend(result, ntest);
				}
			}
		}
	}

	/*
	 * Add partitioning constraints, if requested.
	 */
	if (include_partition && relation->rd_rel->relispartition)
	{
		/* make sure rel->partition_qual is set */
		set_baserel_partition_constraint(relation, rel);
		result = list_concat(result, rel->partition_qual);
	}

	table_close(relation, NoLock);

	return result;
}

/*
 * Try loading data for the statistics object.
 *
 * We don't know if the data (specified by statOid and inh value) exist.
 * The result is stored in stainfos list.
 */
static void
get_relation_statistics_worker(List **stainfos, RelOptInfo *rel,
							   Oid statOid, bool inh,
							   Bitmapset *keys, List *exprs)
{
	Form_pg_statistic_ext_data dataForm;
	HeapTuple	dtup;

	dtup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(statOid), BoolGetDatum(inh));
	if (!HeapTupleIsValid(dtup))
		return;

	dataForm = (Form_pg_statistic_ext_data) GETSTRUCT(dtup);

	/* add one StatisticExtInfo for each kind built */
	if (statext_is_kind_built(dtup, STATS_EXT_NDISTINCT))
	{
		StatisticExtInfo *info = makeNode(StatisticExtInfo);

		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = rel;
		info->kind = STATS_EXT_NDISTINCT;
		info->keys = bms_copy(keys);
		info->exprs = exprs;

		*stainfos = lappend(*stainfos, info);
	}

	if (statext_is_kind_built(dtup, STATS_EXT_DEPENDENCIES))
	{
		StatisticExtInfo *info = makeNode(StatisticExtInfo);

		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = rel;
		info->kind = STATS_EXT_DEPENDENCIES;
		info->keys = bms_copy(keys);
		info->exprs = exprs;

		*stainfos = lappend(*stainfos, info);
	}

	if (statext_is_kind_built(dtup, STATS_EXT_MCV))
	{
		StatisticExtInfo *info = makeNode(StatisticExtInfo);

		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = rel;
		info->kind = STATS_EXT_MCV;
		info->keys = bms_copy(keys);
		info->exprs = exprs;

		*stainfos = lappend(*stainfos, info);
	}

	if (statext_is_kind_built(dtup, STATS_EXT_EXPRESSIONS))
	{
		StatisticExtInfo *info = makeNode(StatisticExtInfo);

		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = rel;
		info->kind = STATS_EXT_EXPRESSIONS;
		info->keys = bms_copy(keys);
		info->exprs = exprs;

		*stainfos = lappend(*stainfos, info);
	}

	ReleaseSysCache(dtup);
}

/*
 * get_relation_statistics
 *		Retrieve extended statistics defined on the table.
 *
 * Returns a List (possibly empty) of StatisticExtInfo objects describing
 * the statistics.  Note that this doesn't load the actual statistics data,
 * just the identifying metadata.  Only stats actually built are considered.
 */
static List *
get_relation_statistics(RelOptInfo *rel, Relation relation)
{
	Index		varno = rel->relid;
	List	   *statoidlist;
	List	   *stainfos = NIL;
	ListCell   *l;

	statoidlist = RelationGetStatExtList(relation);

	foreach(l, statoidlist)
	{
		Oid			statOid = lfirst_oid(l);
		Form_pg_statistic_ext staForm;
		HeapTuple	htup;
		Bitmapset  *keys = NULL;
		List	   *exprs = NIL;
		int			i;

		htup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statOid));
		if (!HeapTupleIsValid(htup))
			elog(ERROR, "cache lookup failed for statistics object %u", statOid);
		staForm = (Form_pg_statistic_ext) GETSTRUCT(htup);

		/*
		 * First, build the array of columns covered.  This is ultimately
		 * wasted if no stats within the object have actually been built, but
		 * it doesn't seem worth troubling over that case.
		 */
		for (i = 0; i < staForm->stxkeys.dim1; i++)
			keys = bms_add_member(keys, staForm->stxkeys.values[i]);

		/*
		 * Preprocess expressions (if any). We read the expressions, run them
		 * through eval_const_expressions, and fix the varnos.
		 *
		 * XXX We don't know yet if there are any data for this stats object,
		 * with either stxdinherit value. But it's reasonable to assume there
		 * is at least one of those, possibly both. So it's better to process
		 * keys and expressions here.
		 */
		{
			bool		isnull;
			Datum		datum;

			/* decode expression (if any) */
			datum = SysCacheGetAttr(STATEXTOID, htup,
									Anum_pg_statistic_ext_stxexprs, &isnull);

			if (!isnull)
			{
				char	   *exprsString;

				exprsString = TextDatumGetCString(datum);
				exprs = (List *) stringToNode(exprsString);
				pfree(exprsString);

				/*
				 * Run the expressions through eval_const_expressions. This is
				 * not just an optimization, but is necessary, because the
				 * planner will be comparing them to similarly-processed qual
				 * clauses, and may fail to detect valid matches without this.
				 * We must not use canonicalize_qual, however, since these
				 * aren't qual expressions.
				 */
				exprs = (List *) eval_const_expressions(NULL, (Node *) exprs);

				/* May as well fix opfuncids too */
				fix_opfuncids((Node *) exprs);

				/*
				 * Modify the copies we obtain from the relcache to have the
				 * correct varno for the parent relation, so that they match
				 * up correctly against qual clauses.
				 */
				if (varno != 1)
					ChangeVarNodes((Node *) exprs, 1, varno, 0);
			}
		}

		/* extract statistics for possible values of stxdinherit flag */

		get_relation_statistics_worker(&stainfos, rel, statOid, true, keys, exprs);

		get_relation_statistics_worker(&stainfos, rel, statOid, false, keys, exprs);

		ReleaseSysCache(htup);
		bms_free(keys);
	}

	list_free(statoidlist);

	return stainfos;
}

/*
 * relation_excluded_by_constraints
 *
 * Detect whether the relation need not be scanned because it has either
 * self-inconsistent restrictions, or restrictions inconsistent with the
 * relation's applicable constraints.
 *
 * Note: this examines only rel->relid, rel->reloptkind, and
 * rel->baserestrictinfo; therefore it can be called before filling in
 * other fields of the RelOptInfo.
 */
bool
relation_excluded_by_constraints(PlannerInfo *root,
								 RelOptInfo *rel, RangeTblEntry *rte)
{
	bool		include_noinherit;
	bool		include_notnull;
	bool		include_partition = false;
	List	   *safe_restrictions;
	List	   *constraint_pred;
	List	   *safe_constraints;
	ListCell   *lc;

	/* As of now, constraint exclusion works only with simple relations. */
	Assert(IS_SIMPLE_REL(rel));

	/*
	 * If there are no base restriction clauses, we have no hope of proving
	 * anything below, so fall out quickly.
	 */
	if (rel->baserestrictinfo == NIL)
		return false;

	/*
	 * Regardless of the setting of constraint_exclusion, detect
	 * constant-FALSE-or-NULL restriction clauses.  Although const-folding
	 * will reduce "anything AND FALSE" to just "FALSE", the baserestrictinfo
	 * list can still have other members besides the FALSE constant, due to
	 * qual pushdown and other mechanisms; so check them all.  This doesn't
	 * fire very often, but it seems cheap enough to be worth doing anyway.
	 * (Without this, we'd miss some optimizations that 9.5 and earlier found
	 * via much more roundabout methods.)
	 */
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Expr	   *clause = rinfo->clause;

		if (clause && IsA(clause, Const) &&
			(((Const *) clause)->constisnull ||
			 !DatumGetBool(((Const *) clause)->constvalue)))
			return true;
	}

	/*
	 * Skip further tests, depending on constraint_exclusion.
	 */
	switch (constraint_exclusion)
	{
		case CONSTRAINT_EXCLUSION_OFF:
			/* In 'off' mode, never make any further tests */
			return false;

		case CONSTRAINT_EXCLUSION_PARTITION:

			/*
			 * When constraint_exclusion is set to 'partition' we only handle
			 * appendrel members.  Partition pruning has already been applied,
			 * so there is no need to consider the rel's partition constraints
			 * here.
			 */
			if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
				break;			/* appendrel member, so process it */
			return false;

		case CONSTRAINT_EXCLUSION_ON:

			/*
			 * In 'on' mode, always apply constraint exclusion.  If we are
			 * considering a baserel that is a partition (i.e., it was
			 * directly named rather than expanded from a parent table), then
			 * its partition constraints haven't been considered yet, so
			 * include them in the processing here.
			 */
			if (rel->reloptkind == RELOPT_BASEREL)
				include_partition = true;
			break;				/* always try to exclude */
	}

	/*
	 * Check for self-contradictory restriction clauses.  We dare not make
	 * deductions with non-immutable functions, but any immutable clauses that
	 * are self-contradictory allow us to conclude the scan is unnecessary.
	 *
	 * Note: strip off RestrictInfo because predicate_refuted_by() isn't
	 * expecting to see any in its predicate argument.
	 */
	safe_restrictions = NIL;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (!contain_mutable_functions((Node *) rinfo->clause))
			safe_restrictions = lappend(safe_restrictions, rinfo->clause);
	}

	/*
	 * We can use weak refutation here, since we're comparing restriction
	 * clauses with restriction clauses.
	 */
	if (predicate_refuted_by(safe_restrictions, safe_restrictions, true))
		return true;

	/*
	 * Only plain relations have constraints, so stop here for other rtekinds.
	 */
	if (rte->rtekind != RTE_RELATION)
		return false;

	/*
	 * If we are scanning just this table, we can use NO INHERIT constraints,
	 * but not if we're scanning its children too.  (Note that partitioned
	 * tables should never have NO INHERIT constraints; but it's not necessary
	 * for us to assume that here.)
	 */
	include_noinherit = !rte->inh;

	/*
	 * Currently, attnotnull constraints must be treated as NO INHERIT unless
	 * this is a partitioned table.  In future we might track their
	 * inheritance status more accurately, allowing this to be refined.
	 *
	 * XXX do we need/want to change this?
	 */
	include_notnull = (!rte->inh || rte->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * Fetch the appropriate set of constraint expressions.
	 */
	constraint_pred = get_relation_constraints(root, rte->relid, rel,
											   include_noinherit,
											   include_notnull,
											   include_partition);

	/*
	 * We do not currently enforce that CHECK constraints contain only
	 * immutable functions, so it's necessary to check here. We daren't draw
	 * conclusions from plan-time evaluation of non-immutable functions. Since
	 * they're ANDed, we can just ignore any mutable constraints in the list,
	 * and reason about the rest.
	 */
	safe_constraints = NIL;
	foreach(lc, constraint_pred)
	{
		Node	   *pred = (Node *) lfirst(lc);

		if (!contain_mutable_functions(pred))
			safe_constraints = lappend(safe_constraints, pred);
	}

	/*
	 * The constraints are effectively ANDed together, so we can just try to
	 * refute the entire collection at once.  This may allow us to make proofs
	 * that would fail if we took them individually.
	 *
	 * Note: we use rel->baserestrictinfo, not safe_restrictions as might seem
	 * an obvious optimization.  Some of the clauses might be OR clauses that
	 * have volatile and nonvolatile subclauses, and it's OK to make
	 * deductions with the nonvolatile parts.
	 *
	 * We need strong refutation because we have to prove that the constraints
	 * would yield false, not just NULL.
	 */
	if (predicate_refuted_by(safe_constraints, rel->baserestrictinfo, false))
		return true;

	return false;
}


/*
 * build_physical_tlist
 *
 * Build a targetlist consisting of exactly the relation's user attributes,
 * in order.  The executor can special-case such tlists to avoid a projection
 * step at runtime, so we use such tlists preferentially for scan nodes.
 *
 * Exception: if there are any dropped or missing columns, we punt and return
 * NIL.  Ideally we would like to handle these cases too.  However this
 * creates problems for ExecTypeFromTL, which may be asked to build a tupdesc
 * for a tlist that includes vars of no-longer-existent types.  In theory we
 * could dig out the required info from the pg_attribute entries of the
 * relation, but that data is not readily available to ExecTypeFromTL.
 * For now, we don't apply the physical-tlist optimization when there are
 * dropped cols.
 *
 * We also support building a "physical" tlist for subqueries, functions,
 * values lists, table expressions, and CTEs, since the same optimization can
 * occur in SubqueryScan, FunctionScan, ValuesScan, CteScan, TableFunc,
 * NamedTuplestoreScan, and WorkTableScan nodes.
 */
List *
build_physical_tlist(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *tlist = NIL;
	Index		varno = rel->relid;
	RangeTblEntry *rte = planner_rt_fetch(varno, root);
	Relation	relation;
	Query	   *subquery;
	Var		   *var;
	ListCell   *l;
	int			attrno,
				numattrs;
	List	   *colvars;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* Assume we already have adequate lock */
			relation = table_open(rte->relid, NoLock);

			numattrs = RelationGetNumberOfAttributes(relation);
			for (attrno = 1; attrno <= numattrs; attrno++)
			{
				Form_pg_attribute att_tup = TupleDescAttr(relation->rd_att,
														  attrno - 1);

				if (att_tup->attisdropped || att_tup->atthasmissing)
				{
					/* found a dropped or missing col, so punt */
					tlist = NIL;
					break;
				}

				var = makeVar(varno,
							  attrno,
							  att_tup->atttypid,
							  att_tup->atttypmod,
							  att_tup->attcollation,
							  0);

				tlist = lappend(tlist,
								makeTargetEntry((Expr *) var,
												attrno,
												NULL,
												false));
			}

			table_close(relation, NoLock);
			break;

		case RTE_SUBQUERY:
			subquery = rte->subquery;
			foreach(l, subquery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);

				/*
				 * A resjunk column of the subquery can be reflected as
				 * resjunk in the physical tlist; we need not punt.
				 */
				var = makeVarFromTargetEntry(varno, tle);

				tlist = lappend(tlist,
								makeTargetEntry((Expr *) var,
												tle->resno,
												NULL,
												tle->resjunk));
			}
			break;

		case RTE_FUNCTION:
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:
			/* Not all of these can have dropped cols, but share code anyway */
			expandRTE(rte, varno, 0, -1, true /* include dropped */ ,
					  NULL, &colvars);
			foreach(l, colvars)
			{
				var = (Var *) lfirst(l);

				/*
				 * A non-Var in expandRTE's output means a dropped column;
				 * must punt.
				 */
				if (!IsA(var, Var))
				{
					tlist = NIL;
					break;
				}

				tlist = lappend(tlist,
								makeTargetEntry((Expr *) var,
												var->varattno,
												NULL,
												false));
			}
			break;

		default:
			/* caller error */
			elog(ERROR, "unsupported RTE kind %d in build_physical_tlist",
				 (int) rte->rtekind);
			break;
	}

	return tlist;
}

/*
 * build_index_tlist
 *
 * Build a targetlist representing the columns of the specified index.
 * Each column is represented by a Var for the corresponding base-relation
 * column, or an expression in base-relation Vars, as appropriate.
 *
 * There are never any dropped columns in indexes, so unlike
 * build_physical_tlist, we need no failure case.
 */
static List *
build_index_tlist(PlannerInfo *root, IndexOptInfo *index,
				  Relation heapRelation)
{
	List	   *tlist = NIL;
	Index		varno = index->rel->relid;
	ListCell   *indexpr_item;
	int			i;

	indexpr_item = list_head(index->indexprs);
	for (i = 0; i < index->ncolumns; i++)
	{
		int			indexkey = index->indexkeys[i];
		Expr	   *indexvar;

		if (indexkey != 0)
		{
			/* simple column */
			const FormData_pg_attribute *att_tup;

			if (indexkey < 0)
				att_tup = SystemAttributeDefinition(indexkey);
			else
				att_tup = TupleDescAttr(heapRelation->rd_att, indexkey - 1);

			indexvar = (Expr *) makeVar(varno,
										indexkey,
										att_tup->atttypid,
										att_tup->atttypmod,
										att_tup->attcollation,
										0);
		}
		else
		{
			/* expression column */
			if (indexpr_item == NULL)
				elog(ERROR, "wrong number of index expressions");
			indexvar = (Expr *) lfirst(indexpr_item);
			indexpr_item = lnext(index->indexprs, indexpr_item);
		}

		tlist = lappend(tlist,
						makeTargetEntry(indexvar,
										i + 1,
										NULL,
										false));
	}
	if (indexpr_item != NULL)
		elog(ERROR, "wrong number of index expressions");

	return tlist;
}

/*
 * restriction_selectivity
 *
 * Returns the selectivity of a specified restriction operator clause.
 * This code executes registered procedures stored in the
 * operator relation, by calling the function manager.
 *
 * See clause_selectivity() for the meaning of the additional parameters.
 */
Selectivity
restriction_selectivity(PlannerInfo *root,
						Oid operatorid,
						List *args,
						Oid inputcollid,
						int varRelid)
{
	RegProcedure oprrest = get_oprrest(operatorid);
	float8		result;

	/*
	 * if the oprrest procedure is missing for whatever reason, use a
	 * selectivity of 0.5
	 */
	if (!oprrest)
		return (Selectivity) 0.5;

	result = DatumGetFloat8(OidFunctionCall4Coll(oprrest,
												 inputcollid,
												 PointerGetDatum(root),
												 ObjectIdGetDatum(operatorid),
												 PointerGetDatum(args),
												 Int32GetDatum(varRelid)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "invalid restriction selectivity: %f", result);

	return (Selectivity) result;
}

/*
 * join_selectivity
 *
 * Returns the selectivity of a specified join operator clause.
 * This code executes registered procedures stored in the
 * operator relation, by calling the function manager.
 *
 * See clause_selectivity() for the meaning of the additional parameters.
 */
Selectivity
join_selectivity(PlannerInfo *root,
				 Oid operatorid,
				 List *args,
				 Oid inputcollid,
				 JoinType jointype,
				 SpecialJoinInfo *sjinfo)
{
	RegProcedure oprjoin = get_oprjoin(operatorid);
	float8		result;

	/*
	 * if the oprjoin procedure is missing for whatever reason, use a
	 * selectivity of 0.5
	 */
	if (!oprjoin)
		return (Selectivity) 0.5;

	result = DatumGetFloat8(OidFunctionCall5Coll(oprjoin,
												 inputcollid,
												 PointerGetDatum(root),
												 ObjectIdGetDatum(operatorid),
												 PointerGetDatum(args),
												 Int16GetDatum(jointype),
												 PointerGetDatum(sjinfo)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "invalid join selectivity: %f", result);

	return (Selectivity) result;
}

/*
 * function_selectivity
 *
 * Returns the selectivity of a specified boolean function clause.
 * This code executes registered procedures stored in the
 * pg_proc relation, by calling the function manager.
 *
 * See clause_selectivity() for the meaning of the additional parameters.
 */
Selectivity
function_selectivity(PlannerInfo *root,
					 Oid funcid,
					 List *args,
					 Oid inputcollid,
					 bool is_join,
					 int varRelid,
					 JoinType jointype,
					 SpecialJoinInfo *sjinfo)
{
	RegProcedure prosupport = get_func_support(funcid);
	SupportRequestSelectivity req;
	SupportRequestSelectivity *sresult;

	/*
	 * If no support function is provided, use our historical default
	 * estimate, 0.3333333.  This seems a pretty unprincipled choice, but
	 * Postgres has been using that estimate for function calls since 1992.
	 * The hoariness of this behavior suggests that we should not be in too
	 * much hurry to use another value.
	 */
	if (!prosupport)
		return (Selectivity) 0.3333333;

	req.type = T_SupportRequestSelectivity;
	req.root = root;
	req.funcid = funcid;
	req.args = args;
	req.inputcollid = inputcollid;
	req.is_join = is_join;
	req.varRelid = varRelid;
	req.jointype = jointype;
	req.sjinfo = sjinfo;
	req.selectivity = -1;		/* to catch failure to set the value */

	sresult = (SupportRequestSelectivity *)
		DatumGetPointer(OidFunctionCall1(prosupport,
										 PointerGetDatum(&req)));

	/* If support function fails, use default */
	if (sresult != &req)
		return (Selectivity) 0.3333333;

	if (req.selectivity < 0.0 || req.selectivity > 1.0)
		elog(ERROR, "invalid function selectivity: %f", req.selectivity);

	return (Selectivity) req.selectivity;
}

/*
 * add_function_cost
 *
 * Get an estimate of the execution cost of a function, and *add* it to
 * the contents of *cost.  The estimate may include both one-time and
 * per-tuple components, since QualCost does.
 *
 * The funcid must always be supplied.  If it is being called as the
 * implementation of a specific parsetree node (FuncExpr, OpExpr,
 * WindowFunc, etc), pass that as "node", else pass NULL.
 *
 * In some usages root might be NULL, too.
 */
void
add_function_cost(PlannerInfo *root, Oid funcid, Node *node,
				  QualCost *cost)
{
	HeapTuple	proctup;
	Form_pg_proc procform;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	if (OidIsValid(procform->prosupport))
	{
		SupportRequestCost req;
		SupportRequestCost *sresult;

		req.type = T_SupportRequestCost;
		req.root = root;
		req.funcid = funcid;
		req.node = node;

		/* Initialize cost fields so that support function doesn't have to */
		req.startup = 0;
		req.per_tuple = 0;

		sresult = (SupportRequestCost *)
			DatumGetPointer(OidFunctionCall1(procform->prosupport,
											 PointerGetDatum(&req)));

		if (sresult == &req)
		{
			/* Success, so accumulate support function's estimate into *cost */
			cost->startup += req.startup;
			cost->per_tuple += req.per_tuple;
			ReleaseSysCache(proctup);
			return;
		}
	}

	/* No support function, or it failed, so rely on procost */
	cost->per_tuple += procform->procost * cpu_operator_cost;

	ReleaseSysCache(proctup);
}

/*
 * get_function_rows
 *
 * Get an estimate of the number of rows returned by a set-returning function.
 *
 * The funcid must always be supplied.  In current usage, the calling node
 * will always be supplied, and will be either a FuncExpr or OpExpr.
 * But it's a good idea to not fail if it's NULL.
 *
 * In some usages root might be NULL, too.
 *
 * Note: this returns the unfiltered result of the support function, if any.
 * It's usually a good idea to apply clamp_row_est() to the result, but we
 * leave it to the caller to do so.
 */
double
get_function_rows(PlannerInfo *root, Oid funcid, Node *node)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	double		result;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	Assert(procform->proretset);	/* else caller error */

	if (OidIsValid(procform->prosupport))
	{
		SupportRequestRows req;
		SupportRequestRows *sresult;

		req.type = T_SupportRequestRows;
		req.root = root;
		req.funcid = funcid;
		req.node = node;

		req.rows = 0;			/* just for sanity */

		sresult = (SupportRequestRows *)
			DatumGetPointer(OidFunctionCall1(procform->prosupport,
											 PointerGetDatum(&req)));

		if (sresult == &req)
		{
			/* Success */
			ReleaseSysCache(proctup);
			return req.rows;
		}
	}

	/* No support function, or it failed, so rely on prorows */
	result = procform->prorows;

	ReleaseSysCache(proctup);

	return result;
}

/*
 * has_unique_index
 *
 * Detect whether there is a unique index on the specified attribute
 * of the specified relation, thus allowing us to conclude that all
 * the (non-null) values of the attribute are distinct.
 *
 * This function does not check the index's indimmediate property, which
 * means that uniqueness may transiently fail to hold intra-transaction.
 * That's appropriate when we are making statistical estimates, but beware
 * of using this for any correctness proofs.
 */
bool
has_unique_index(RelOptInfo *rel, AttrNumber attno)
{
	ListCell   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);

		/*
		 * Note: ignore partial indexes, since they don't allow us to conclude
		 * that all attr values are distinct, *unless* they are marked predOK
		 * which means we know the index's predicate is satisfied by the
		 * query. We don't take any interest in expressional indexes either.
		 * Also, a multicolumn unique index doesn't allow us to conclude that
		 * just the specified attr is unique.
		 */
		if (index->unique &&
			index->nkeycolumns == 1 &&
			index->indexkeys[0] == attno &&
			(index->indpred == NIL || index->predOK))
			return true;
	}
	return false;
}


/*
 * has_row_triggers
 *
 * Detect whether the specified relation has any row-level triggers for event.
 */
bool
has_row_triggers(PlannerInfo *root, Index rti, CmdType event)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	Relation	relation;
	TriggerDesc *trigDesc;
	bool		result = false;

	/* Assume we already have adequate lock */
	relation = table_open(rte->relid, NoLock);

	trigDesc = relation->trigdesc;
	switch (event)
	{
		case CMD_INSERT:
			if (trigDesc &&
				(trigDesc->trig_insert_after_row ||
				 trigDesc->trig_insert_before_row))
				result = true;
			break;
		case CMD_UPDATE:
			if (trigDesc &&
				(trigDesc->trig_update_after_row ||
				 trigDesc->trig_update_before_row))
				result = true;
			break;
		case CMD_DELETE:
			if (trigDesc &&
				(trigDesc->trig_delete_after_row ||
				 trigDesc->trig_delete_before_row))
				result = true;
			break;
			/* There is no separate event for MERGE, only INSERT/UPDATE/DELETE */
		case CMD_MERGE:
			result = false;
			break;
		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) event);
			break;
	}

	table_close(relation, NoLock);
	return result;
}

/*
 * has_stored_generated_columns
 *
 * Does table identified by RTI have any STORED GENERATED columns?
 */
bool
has_stored_generated_columns(PlannerInfo *root, Index rti)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	Relation	relation;
	TupleDesc	tupdesc;
	bool		result = false;

	/* Assume we already have adequate lock */
	relation = table_open(rte->relid, NoLock);

	tupdesc = RelationGetDescr(relation);
	result = tupdesc->constr && tupdesc->constr->has_generated_stored;

	table_close(relation, NoLock);

	return result;
}

/*
 * get_dependent_generated_columns
 *
 * Get the column numbers of any STORED GENERATED columns of the relation
 * that depend on any column listed in target_cols.  Both the input and
 * result bitmapsets contain column numbers offset by
 * FirstLowInvalidHeapAttributeNumber.
 */
Bitmapset *
get_dependent_generated_columns(PlannerInfo *root, Index rti,
								Bitmapset *target_cols)
{
	Bitmapset  *dependentCols = NULL;
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	Relation	relation;
	TupleDesc	tupdesc;
	TupleConstr *constr;

	/* Assume we already have adequate lock */
	relation = table_open(rte->relid, NoLock);

	tupdesc = RelationGetDescr(relation);
	constr = tupdesc->constr;

	if (constr && constr->has_generated_stored)
	{
		for (int i = 0; i < constr->num_defval; i++)
		{
			AttrDefault *defval = &constr->defval[i];
			Node	   *expr;
			Bitmapset  *attrs_used = NULL;

			/* skip if not generated column */
			if (!TupleDescAttr(tupdesc, defval->adnum - 1)->attgenerated)
				continue;

			/* identify columns this generated column depends on */
			expr = stringToNode(defval->adbin);
			pull_varattnos(expr, 1, &attrs_used);

			if (bms_overlap(target_cols, attrs_used))
				dependentCols = bms_add_member(dependentCols,
											   defval->adnum - FirstLowInvalidHeapAttributeNumber);
		}
	}

	table_close(relation, NoLock);

	return dependentCols;
}

/*
 * set_relation_partition_info
 *
 * Set partitioning scheme and related information for a partitioned table.
 */
static void
set_relation_partition_info(PlannerInfo *root, RelOptInfo *rel,
							Relation relation)
{
	PartitionDesc partdesc;

	/*
	 * Create the PartitionDirectory infrastructure if we didn't already.
	 */
	if (root->glob->partition_directory == NULL)
	{
		root->glob->partition_directory =
			CreatePartitionDirectory(CurrentMemoryContext, true);
	}

	partdesc = PartitionDirectoryLookup(root->glob->partition_directory,
										relation);
	rel->part_scheme = find_partition_scheme(root, relation);
	Assert(partdesc != NULL && rel->part_scheme != NULL);
	rel->boundinfo = partdesc->boundinfo;
	rel->nparts = partdesc->nparts;
	set_baserel_partition_key_exprs(relation, rel);
	set_baserel_partition_constraint(relation, rel);
}

/*
 * find_partition_scheme
 *
 * Find or create a PartitionScheme for this Relation.
 */
static PartitionScheme
find_partition_scheme(PlannerInfo *root, Relation relation)
{
	PartitionKey partkey = RelationGetPartitionKey(relation);
	ListCell   *lc;
	int			partnatts,
				i;
	PartitionScheme part_scheme;

	/* A partitioned table should have a partition key. */
	Assert(partkey != NULL);

	partnatts = partkey->partnatts;

	/* Search for a matching partition scheme and return if found one. */
	foreach(lc, root->part_schemes)
	{
		part_scheme = lfirst(lc);

		/* Match partitioning strategy and number of keys. */
		if (partkey->strategy != part_scheme->strategy ||
			partnatts != part_scheme->partnatts)
			continue;

		/* Match partition key type properties. */
		if (memcmp(partkey->partopfamily, part_scheme->partopfamily,
				   sizeof(Oid) * partnatts) != 0 ||
			memcmp(partkey->partopcintype, part_scheme->partopcintype,
				   sizeof(Oid) * partnatts) != 0 ||
			memcmp(partkey->partcollation, part_scheme->partcollation,
				   sizeof(Oid) * partnatts) != 0)
			continue;

		/*
		 * Length and byval information should match when partopcintype
		 * matches.
		 */
		Assert(memcmp(partkey->parttyplen, part_scheme->parttyplen,
					  sizeof(int16) * partnatts) == 0);
		Assert(memcmp(partkey->parttypbyval, part_scheme->parttypbyval,
					  sizeof(bool) * partnatts) == 0);

		/*
		 * If partopfamily and partopcintype matched, must have the same
		 * partition comparison functions.  Note that we cannot reliably
		 * Assert the equality of function structs themselves for they might
		 * be different across PartitionKey's, so just Assert for the function
		 * OIDs.
		 */
#ifdef USE_ASSERT_CHECKING
		for (i = 0; i < partkey->partnatts; i++)
			Assert(partkey->partsupfunc[i].fn_oid ==
				   part_scheme->partsupfunc[i].fn_oid);
#endif

		/* Found matching partition scheme. */
		return part_scheme;
	}

	/*
	 * Did not find matching partition scheme. Create one copying relevant
	 * information from the relcache. We need to copy the contents of the
	 * array since the relcache entry may not survive after we have closed the
	 * relation.
	 */
	part_scheme = (PartitionScheme) palloc0(sizeof(PartitionSchemeData));
	part_scheme->strategy = partkey->strategy;
	part_scheme->partnatts = partkey->partnatts;

	part_scheme->partopfamily = (Oid *) palloc(sizeof(Oid) * partnatts);
	memcpy(part_scheme->partopfamily, partkey->partopfamily,
		   sizeof(Oid) * partnatts);

	part_scheme->partopcintype = (Oid *) palloc(sizeof(Oid) * partnatts);
	memcpy(part_scheme->partopcintype, partkey->partopcintype,
		   sizeof(Oid) * partnatts);

	part_scheme->partcollation = (Oid *) palloc(sizeof(Oid) * partnatts);
	memcpy(part_scheme->partcollation, partkey->partcollation,
		   sizeof(Oid) * partnatts);

	part_scheme->parttyplen = (int16 *) palloc(sizeof(int16) * partnatts);
	memcpy(part_scheme->parttyplen, partkey->parttyplen,
		   sizeof(int16) * partnatts);

	part_scheme->parttypbyval = (bool *) palloc(sizeof(bool) * partnatts);
	memcpy(part_scheme->parttypbyval, partkey->parttypbyval,
		   sizeof(bool) * partnatts);

	part_scheme->partsupfunc = (FmgrInfo *)
		palloc(sizeof(FmgrInfo) * partnatts);
	for (i = 0; i < partnatts; i++)
		fmgr_info_copy(&part_scheme->partsupfunc[i], &partkey->partsupfunc[i],
					   CurrentMemoryContext);

	/* Add the partitioning scheme to PlannerInfo. */
	root->part_schemes = lappend(root->part_schemes, part_scheme);

	return part_scheme;
}

/*
 * set_baserel_partition_key_exprs
 *
 * Builds partition key expressions for the given base relation and fills
 * rel->partexprs.
 */
static void
set_baserel_partition_key_exprs(Relation relation,
								RelOptInfo *rel)
{
	PartitionKey partkey = RelationGetPartitionKey(relation);
	int			partnatts;
	int			cnt;
	List	  **partexprs;
	ListCell   *lc;
	Index		varno = rel->relid;

	Assert(IS_SIMPLE_REL(rel) && rel->relid > 0);

	/* A partitioned table should have a partition key. */
	Assert(partkey != NULL);

	partnatts = partkey->partnatts;
	partexprs = (List **) palloc(sizeof(List *) * partnatts);
	lc = list_head(partkey->partexprs);

	for (cnt = 0; cnt < partnatts; cnt++)
	{
		Expr	   *partexpr;
		AttrNumber	attno = partkey->partattrs[cnt];

		if (attno != InvalidAttrNumber)
		{
			/* Single column partition key is stored as a Var node. */
			Assert(attno > 0);

			partexpr = (Expr *) makeVar(varno, attno,
										partkey->parttypid[cnt],
										partkey->parttypmod[cnt],
										partkey->parttypcoll[cnt], 0);
		}
		else
		{
			if (lc == NULL)
				elog(ERROR, "wrong number of partition key expressions");

			/* Re-stamp the expression with given varno. */
			partexpr = (Expr *) copyObject(lfirst(lc));
			ChangeVarNodes((Node *) partexpr, 1, varno, 0);
			lc = lnext(partkey->partexprs, lc);
		}

		/* Base relations have a single expression per key. */
		partexprs[cnt] = list_make1(partexpr);
	}

	rel->partexprs = partexprs;

	/*
	 * A base relation does not have nullable partition key expressions, since
	 * no outer join is involved.  We still allocate an array of empty
	 * expression lists to keep partition key expression handling code simple.
	 * See build_joinrel_partition_info() and match_expr_to_partition_keys().
	 */
	rel->nullable_partexprs = (List **) palloc0(sizeof(List *) * partnatts);
}

/*
 * set_baserel_partition_constraint
 *
 * Builds the partition constraint for the given base relation and sets it
 * in the given RelOptInfo.  All Var nodes are restamped with the relid of the
 * given relation.
 */
static void
set_baserel_partition_constraint(Relation relation, RelOptInfo *rel)
{
	List	   *partconstr;

	if (rel->partition_qual)	/* already done */
		return;

	/*
	 * Run the partition quals through const-simplification similar to check
	 * constraints.  We skip canonicalize_qual, though, because partition
	 * quals should be in canonical form already; also, since the qual is in
	 * implicit-AND format, we'd have to explicitly convert it to explicit-AND
	 * format and back again.
	 */
	partconstr = RelationGetPartitionQual(relation);
	if (partconstr)
	{
		partconstr = (List *) expression_planner((Expr *) partconstr);
		if (rel->relid != 1)
			ChangeVarNodes((Node *) partconstr, 1, rel->relid, 0);
		rel->partition_qual = partconstr;
	}
}
