/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"
#include "optimizer/predtest.h"
#include "optimizer/prep.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


/* GUC parameter */
int			constraint_exclusion = CONSTRAINT_EXCLUSION_PARTITION;

/* Hook for plugins to get control in get_relation_info() */
get_relation_info_hook_type get_relation_info_hook = NULL;


static int32 get_rel_data_width(Relation rel, int32 *attr_widths);
static List *get_relation_constraints(PlannerInfo *root,
						 Oid relationObjectId, RelOptInfo *rel,
						 bool include_notnull);
static List *build_index_tlist(PlannerInfo *root, IndexOptInfo *index,
				  Relation heapRelation);


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
 *	fdwroutine	if it's a foreign table, the FDW function pointers
 *	pages		number of pages
 *	tuples		number of tuples
 *
 * Also, initialize the attr_needed[] and attr_widths[] arrays.  In most
 * cases these are left as zeroes, but sometimes we need to compute attr
 * widths here, and we may as well cache the results for costsize.c.
 *
 * If inhparent is true, all we need to do is set up the attr arrays:
 * the RelOptInfo actually represents the appendrel formed by an inheritance
 * tree, and so the parent rel's physical size and index information isn't
 * important for it.
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
	relation = heap_open(relationObjectId, NoLock);

	/* Temporary and unlogged relations are inaccessible during recovery. */
	if (!RelationNeedsWAL(relation) && RecoveryInProgress())
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
	 * Estimate relation size --- unless it's an inheritance parent, in which
	 * case the size will be computed later in set_append_rel_pathlist, and we
	 * must leave it zero for now to avoid bollixing the total_table_pages
	 * calculation.
	 */
	if (!inhparent)
		estimate_rel_size(relation, rel->attr_widths - rel->min_attr,
						  &rel->pages, &rel->tuples, &rel->allvisfrac);

	/*
	 * Make list of indexes.  Ignore indexes on system catalogs if told to.
	 * Don't bother with indexes for an inheritance parent, either.
	 */
	if (inhparent ||
		(IgnoreSystemIndexes && IsSystemClass(relation->rd_rel)))
		hasindex = false;
	else
		hasindex = relation->rd_rel->relhasindex;

	if (hasindex)
	{
		List	   *indexoidlist;
		ListCell   *l;
		LOCKMODE	lmode;

		indexoidlist = RelationGetIndexList(relation);

		/*
		 * For each index, we get the same type of lock that the executor will
		 * need, and do not release it.  This saves a couple of trips to the
		 * shared lock manager while not creating any real loss of
		 * concurrency, because no schema changes could be happening on the
		 * index while we hold lock on the parent rel, and neither lock type
		 * blocks any other kind of index operation.
		 */
		if (rel->relid == root->parse->resultRelation)
			lmode = RowExclusiveLock;
		else
			lmode = AccessShareLock;

		foreach(l, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(l);
			Relation	indexRelation;
			Form_pg_index index;
			IndexOptInfo *info;
			int			ncolumns;
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
			 * IndexIsReady.
			 */
			if (!IndexIsValid(index))
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
			info->indexkeys = (int *) palloc(sizeof(int) * ncolumns);
			info->indexcollations = (Oid *) palloc(sizeof(Oid) * ncolumns);
			info->opfamily = (Oid *) palloc(sizeof(Oid) * ncolumns);
			info->opcintype = (Oid *) palloc(sizeof(Oid) * ncolumns);

			for (i = 0; i < ncolumns; i++)
			{
				info->indexkeys[i] = index->indkey.values[i];
				info->indexcollations[i] = indexRelation->rd_indcollation[i];
				info->opfamily[i] = indexRelation->rd_opfamily[i];
				info->opcintype[i] = indexRelation->rd_opcintype[i];
			}

			info->relam = indexRelation->rd_rel->relam;
			info->amcostestimate = indexRelation->rd_am->amcostestimate;
			info->canreturn = index_can_return(indexRelation);
			info->amcanorderbyop = indexRelation->rd_am->amcanorderbyop;
			info->amoptionalkey = indexRelation->rd_am->amoptionalkey;
			info->amsearcharray = indexRelation->rd_am->amsearcharray;
			info->amsearchnulls = indexRelation->rd_am->amsearchnulls;
			info->amhasgettuple = OidIsValid(indexRelation->rd_am->amgettuple);
			info->amhasgetbitmap = OidIsValid(indexRelation->rd_am->amgetbitmap);

			/*
			 * Fetch the ordering information for the index, if any.
			 */
			if (info->relam == BTREE_AM_OID)
			{
				/*
				 * If it's a btree index, we can use its opfamily OIDs
				 * directly as the sort ordering opfamily OIDs.
				 */
				Assert(indexRelation->rd_am->amcanorder);

				info->sortopfamily = info->opfamily;
				info->reverse_sort = (bool *) palloc(sizeof(bool) * ncolumns);
				info->nulls_first = (bool *) palloc(sizeof(bool) * ncolumns);

				for (i = 0; i < ncolumns; i++)
				{
					int16		opt = indexRelation->rd_indoption[i];

					info->reverse_sort[i] = (opt & INDOPTION_DESC) != 0;
					info->nulls_first[i] = (opt & INDOPTION_NULLS_FIRST) != 0;
				}
			}
			else if (indexRelation->rd_am->amcanorder)
			{
				/*
				 * Otherwise, identify the corresponding btree opfamilies by
				 * trying to map this index's "<" operators into btree.  Since
				 * "<" uniquely defines the behavior of a sort order, this is
				 * a sufficient test.
				 *
				 * XXX This method is rather slow and also requires the
				 * undesirable assumption that the other index AM numbers its
				 * strategies the same as btree.  It'd be better to have a way
				 * to explicitly declare the corresponding btree opfamily for
				 * each opfamily of the other index type.  But given the lack
				 * of current or foreseeable amcanorder index types, it's not
				 * worth expending more effort on now.
				 */
				info->sortopfamily = (Oid *) palloc(sizeof(Oid) * ncolumns);
				info->reverse_sort = (bool *) palloc(sizeof(bool) * ncolumns);
				info->nulls_first = (bool *) palloc(sizeof(bool) * ncolumns);

				for (i = 0; i < ncolumns; i++)
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

			info->predOK = false;		/* set later in indxpath.c */
			info->unique = index->indisunique;
			info->immediate = index->indimmediate;
			info->hypothetical = false;

			/*
			 * Estimate the index size.  If it's not a partial index, we lock
			 * the number-of-tuples estimate to equal the parent table; if it
			 * is partial then we have to use the same methods as we would for
			 * a table, except we can be sure that the index is not larger
			 * than the table.
			 */
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

			if (info->relam == BTREE_AM_OID)
			{
				/* For btrees, get tree height while we have the index open */
				info->tree_height = _bt_getrootheight(indexRelation);
			}
			else
			{
				/* For other index types, just set it to "unknown" for now */
				info->tree_height = -1;
			}

			index_close(indexRelation, NoLock);

			indexinfos = lcons(info, indexinfos);
		}

		list_free(indexoidlist);
	}

	rel->indexlist = indexinfos;

	/* Grab the fdwroutine info using the relcache, while we have it */
	if (relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		rel->fdwroutine = GetFdwRoutineForRelation(relation, true);
	else
		rel->fdwroutine = NULL;

	heap_close(relation, NoLock);

	/*
	 * Allow a plugin to editorialize on the info we obtained from the
	 * catalogs.  Actions might include altering the assumed relation size,
	 * removing an index, or adding a hypothetical index to the indexlist.
	 */
	if (get_relation_info_hook)
		(*get_relation_info_hook) (root, relationObjectId, inhparent, rel);
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

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_INDEX:
		case RELKIND_MATVIEW:
		case RELKIND_TOASTVALUE:
			/* it has storage, ok to call the smgr */
			curpages = RelationGetNumberOfBlocks(rel);

			/*
			 * HACK: if the relation has never yet been vacuumed, use a
			 * minimum size estimate of 10 pages.  The idea here is to avoid
			 * assuming a newly-created table is really small, even if it
			 * currently is, because that may not be true once some data gets
			 * loaded into it.	Once a vacuum or analyze cycle has been done
			 * on it, it's more reasonable to believe the size is somewhat
			 * stable.
			 *
			 * (Note that this is only an issue if the plan gets cached and
			 * used again after the table has been filled.	What we're trying
			 * to avoid is using a nestloop-type plan on a table that has
			 * grown substantially since the plan was made.  Normally,
			 * autovacuum/autoanalyze will occur once enough inserts have
			 * happened and cause cached-plan invalidation; but that doesn't
			 * happen instantaneously, and it won't happen at all for cases
			 * such as temporary tables.)
			 *
			 * We approximate "never vacuumed" by "has relpages = 0", which
			 * means this will also fire on genuinely empty relations.	Not
			 * great, but fortunately that's a seldom-seen case in the real
			 * world, and it shouldn't degrade the quality of the plan too
			 * much anyway to err in this direction.
			 *
			 * There are two exceptions wherein we don't apply this heuristic.
			 * One is if the table has inheritance children.  Totally empty
			 * parent tables are quite common, so we should be willing to
			 * believe that they are empty.  Also, we don't apply the 10-page
			 * minimum to indexes.
			 */
			if (curpages < 10 &&
				rel->rd_rel->relpages == 0 &&
				!rel->rd_rel->relhassubclass &&
				rel->rd_rel->relkind != RELKIND_INDEX)
				curpages = 10;

			/* report estimated # pages */
			*pages = curpages;
			/* quick exit if rel is clearly empty */
			if (curpages == 0)
			{
				*tuples = 0;
				*allvisfrac = 0;
				break;
			}
			/* coerce values in pg_class to more desirable types */
			relpages = (BlockNumber) rel->rd_rel->relpages;
			reltuples = (double) rel->rd_rel->reltuples;
			relallvisible = (BlockNumber) rel->rd_rel->relallvisible;

			/*
			 * If it's an index, discount the metapage while estimating the
			 * number of tuples.  This is a kluge because it assumes more than
			 * it ought to about index structure.  Currently it's OK for
			 * btree, hash, and GIN indexes but suspect for GiST indexes.
			 */
			if (rel->rd_rel->relkind == RELKIND_INDEX &&
				relpages > 0)
			{
				curpages--;
				relpages--;
			}

			/* estimate number of tuples from previous tuple density */
			if (relpages > 0)
				density = reltuples / (double) relpages;
			else
			{
				/*
				 * When we have no data because the relation was truncated,
				 * estimate tuple width from attribute datatypes.  We assume
				 * here that the pages are completely full, which is OK for
				 * tables (since they've presumably not been VACUUMed yet) but
				 * is probably an overestimate for indexes.  Fortunately
				 * get_relation_info() can clamp the overestimate to the
				 * parent table's size.
				 *
				 * Note: this code intentionally disregards alignment
				 * considerations, because (a) that would be gilding the lily
				 * considering how crude the estimate is, and (b) it creates
				 * platform dependencies in the default plans which are kind
				 * of a headache for regression testing.
				 */
				int32		tuple_width;

				tuple_width = get_rel_data_width(rel, attr_widths);
				tuple_width += sizeof(HeapTupleHeaderData);
				tuple_width += sizeof(ItemPointerData);
				/* note: integer division is intentional here */
				density = (BLCKSZ - SizeOfPageHeaderData) / tuple_width;
			}
			*tuples = rint(density * (double) curpages);

			/*
			 * We use relallvisible as-is, rather than scaling it up like we
			 * do for the pages and tuples counts, on the theory that any
			 * pages added since the last VACUUM are most likely not marked
			 * all-visible.  But costsize.c wants it converted to a fraction.
			 */
			if (relallvisible == 0 || curpages <= 0)
				*allvisfrac = 0;
			else if ((double) relallvisible >= curpages)
				*allvisfrac = 1;
			else
				*allvisfrac = (double) relallvisible / curpages;
			break;
		case RELKIND_SEQUENCE:
			/* Sequences always have a known size */
			*pages = 1;
			*tuples = 1;
			*allvisfrac = 0;
			break;
		case RELKIND_FOREIGN_TABLE:
			/* Just use whatever's in pg_class */
			*pages = rel->rd_rel->relpages;
			*tuples = rel->rd_rel->reltuples;
			*allvisfrac = 0;
			break;
		default:
			/* else it has no disk storage; probably shouldn't get here? */
			*pages = 0;
			*tuples = 0;
			*allvisfrac = 0;
			break;
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
static int32
get_rel_data_width(Relation rel, int32 *attr_widths)
{
	int32		tuple_width = 0;
	int			i;

	for (i = 1; i <= RelationGetNumberOfAttributes(rel); i++)
	{
		Form_pg_attribute att = rel->rd_att->attrs[i - 1];
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

	return tuple_width;
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
	relation = heap_open(relid, NoLock);

	result = get_rel_data_width(relation, attr_widths);

	heap_close(relation, NoLock);

	return result;
}


/*
 * get_relation_constraints
 *
 * Retrieve the validated CHECK constraint expressions of the given relation.
 *
 * Returns a List (possibly empty) of constraint expressions.  Each one
 * has been canonicalized, and its Vars are changed to have the varno
 * indicated by rel->relid.  This allows the expressions to be easily
 * compared to expressions taken from WHERE.
 *
 * If include_notnull is true, "col IS NOT NULL" expressions are generated
 * and added to the result for each column that's marked attnotnull.
 *
 * Note: at present this is invoked at most once per relation per planner
 * run, and in many cases it won't be invoked at all, so there seems no
 * point in caching the data in RelOptInfo.
 */
static List *
get_relation_constraints(PlannerInfo *root,
						 Oid relationObjectId, RelOptInfo *rel,
						 bool include_notnull)
{
	List	   *result = NIL;
	Index		varno = rel->relid;
	Relation	relation;
	TupleConstr *constr;

	/*
	 * We assume the relation has already been safely locked.
	 */
	relation = heap_open(relationObjectId, NoLock);

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
			 * ignore it here.
			 */
			if (!constr->check[i].ccvalid)
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

			cexpr = (Node *) canonicalize_qual((Expr *) cexpr);

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
				Form_pg_attribute att = relation->rd_att->attrs[i - 1];

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
					ntest->argisrow = type_is_rowtype(att->atttypid);
					result = lappend(result, ntest);
				}
			}
		}
	}

	heap_close(relation, NoLock);

	return result;
}


/*
 * relation_excluded_by_constraints
 *
 * Detect whether the relation need not be scanned because it has either
 * self-inconsistent restrictions, or restrictions inconsistent with the
 * relation's validated CHECK constraints.
 *
 * Note: this examines only rel->relid, rel->reloptkind, and
 * rel->baserestrictinfo; therefore it can be called before filling in
 * other fields of the RelOptInfo.
 */
bool
relation_excluded_by_constraints(PlannerInfo *root,
								 RelOptInfo *rel, RangeTblEntry *rte)
{
	List	   *safe_restrictions;
	List	   *constraint_pred;
	List	   *safe_constraints;
	ListCell   *lc;

	/* Skip the test if constraint exclusion is disabled for the rel */
	if (constraint_exclusion == CONSTRAINT_EXCLUSION_OFF ||
		(constraint_exclusion == CONSTRAINT_EXCLUSION_PARTITION &&
		 !(rel->reloptkind == RELOPT_OTHER_MEMBER_REL ||
		   (root->hasInheritedTarget &&
			rel->reloptkind == RELOPT_BASEREL &&
			rel->relid == root->parse->resultRelation))))
		return false;

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

	if (predicate_refuted_by(safe_restrictions, safe_restrictions))
		return true;

	/* Only plain relations have constraints */
	if (rte->rtekind != RTE_RELATION || rte->inh)
		return false;

	/*
	 * OK to fetch the constraint expressions.	Include "col IS NOT NULL"
	 * expressions for attnotnull columns, in case we can refute those.
	 */
	constraint_pred = get_relation_constraints(root, rte->relid, rel, true);

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
	 */
	if (predicate_refuted_by(safe_constraints, rel->baserestrictinfo))
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
 * Exception: if there are any dropped columns, we punt and return NIL.
 * Ideally we would like to handle the dropped-column case too.  However this
 * creates problems for ExecTypeFromTL, which may be asked to build a tupdesc
 * for a tlist that includes vars of no-longer-existent types.	In theory we
 * could dig out the required info from the pg_attribute entries of the
 * relation, but that data is not readily available to ExecTypeFromTL.
 * For now, we don't apply the physical-tlist optimization when there are
 * dropped cols.
 *
 * We also support building a "physical" tlist for subqueries, functions,
 * values lists, and CTEs, since the same optimization can occur in
 * SubqueryScan, FunctionScan, ValuesScan, CteScan, and WorkTableScan nodes.
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
			relation = heap_open(rte->relid, NoLock);

			numattrs = RelationGetNumberOfAttributes(relation);
			for (attrno = 1; attrno <= numattrs; attrno++)
			{
				Form_pg_attribute att_tup = relation->rd_att->attrs[attrno - 1];

				if (att_tup->attisdropped)
				{
					/* found a dropped col, so punt */
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

			heap_close(relation, NoLock);
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
		case RTE_VALUES:
		case RTE_CTE:
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
			Form_pg_attribute att_tup;

			if (indexkey < 0)
				att_tup = SystemAttributeDefinition(indexkey,
										   heapRelation->rd_rel->relhasoids);
			else
				att_tup = heapRelation->rd_att->attrs[indexkey - 1];

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
			indexpr_item = lnext(indexpr_item);
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
			index->ncolumns == 1 &&
			index->indexkeys[0] == attno &&
			(index->indpred == NIL || index->predOK))
			return true;
	}
	return false;
}
