/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/plancat.c,v 1.114.2.1 2005/11/22 18:23:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_index.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "catalog/catalog.h"
#include "miscadmin.h"


static void estimate_rel_size(Relation rel, int32 *attr_widths,
				  BlockNumber *pages, double *tuples);


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
 *	pages		number of pages
 *	tuples		number of tuples
 *
 * Also, initialize the attr_needed[] and attr_widths[] arrays.  In most
 * cases these are left as zeroes, but sometimes we need to compute attr
 * widths here, and we may as well cache the results for costsize.c.
 */
void
get_relation_info(Oid relationObjectId, RelOptInfo *rel)
{
	Index		varno = rel->relid;
	Relation	relation;
	bool		hasindex;
	List	   *indexinfos = NIL;

	/*
	 * Normally, we can assume the rewriter already acquired at least
	 * AccessShareLock on each relation used in the query.	However this will
	 * not be the case for relations added to the query because they are
	 * inheritance children of some relation mentioned explicitly. For them,
	 * this is the first access during the parse/rewrite/plan pipeline, and so
	 * we need to obtain and keep a suitable lock.
	 *
	 * XXX really, a suitable lock is RowShareLock if the relation is an
	 * UPDATE/DELETE target, and AccessShareLock otherwise.  However we cannot
	 * easily tell here which to get, so for the moment just get
	 * AccessShareLock always.	The executor will get the right lock when it
	 * runs, which means there is a very small chance of deadlock trying to
	 * upgrade our lock.
	 */
	if (rel->reloptkind == RELOPT_BASEREL)
		relation = heap_open(relationObjectId, NoLock);
	else
		relation = heap_open(relationObjectId, AccessShareLock);

	rel->min_attr = FirstLowInvalidHeapAttributeNumber + 1;
	rel->max_attr = RelationGetNumberOfAttributes(relation);

	Assert(rel->max_attr >= rel->min_attr);
	rel->attr_needed = (Relids *)
		palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(Relids));
	rel->attr_widths = (int32 *)
		palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(int32));

	/*
	 * Estimate relation size.
	 */
	estimate_rel_size(relation, rel->attr_widths - rel->min_attr,
					  &rel->pages, &rel->tuples);

	/*
	 * Make list of indexes.  Ignore indexes on system catalogs if told to.
	 */
	if (IsIgnoringSystemIndexes() && IsSystemClass(relation->rd_rel))
		hasindex = false;
	else
		hasindex = relation->rd_rel->relhasindex;

	if (hasindex)
	{
		List	   *indexoidlist;
		ListCell   *l;

		indexoidlist = RelationGetIndexList(relation);

		foreach(l, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(l);
			Relation	indexRelation;
			Form_pg_index index;
			IndexOptInfo *info;
			int			ncolumns;
			int			i;
			int16		amorderstrategy;

			/*
			 * Extract info from the relation descriptor for the index.
			 *
			 * Note that we take no lock on the index; we assume our lock on
			 * the parent table will protect the index's schema information.
			 * When and if the executor actually uses the index, it will take
			 * a lock as needed to protect the access to the index contents.
			 */
			indexRelation = index_open(indexoid);
			index = indexRelation->rd_index;

			info = makeNode(IndexOptInfo);

			info->indexoid = index->indexrelid;
			info->rel = rel;
			info->ncolumns = ncolumns = index->indnatts;

			/*
			 * Need to make classlist and ordering arrays large enough to put
			 * a terminating 0 at the end of each one.
			 */
			info->indexkeys = (int *) palloc(sizeof(int) * ncolumns);
			info->classlist = (Oid *) palloc0(sizeof(Oid) * (ncolumns + 1));
			info->ordering = (Oid *) palloc0(sizeof(Oid) * (ncolumns + 1));

			for (i = 0; i < ncolumns; i++)
			{
				info->classlist[i] = indexRelation->rd_indclass->values[i];
				info->indexkeys[i] = index->indkey.values[i];
			}

			info->relam = indexRelation->rd_rel->relam;
			info->amcostestimate = indexRelation->rd_am->amcostestimate;
			info->amoptionalkey = indexRelation->rd_am->amoptionalkey;

			/*
			 * Fetch the ordering operators associated with the index, if any.
			 */
			amorderstrategy = indexRelation->rd_am->amorderstrategy;
			if (amorderstrategy != 0)
			{
				int			oprindex = amorderstrategy - 1;

				for (i = 0; i < ncolumns; i++)
				{
					info->ordering[i] = indexRelation->rd_operator[oprindex];
					oprindex += indexRelation->rd_am->amstrategies;
				}
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
			info->predOK = false;		/* set later in indxpath.c */
			info->unique = index->indisunique;

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
				estimate_rel_size(indexRelation, NULL,
								  &info->pages, &info->tuples);
				if (info->tuples > rel->tuples)
					info->tuples = rel->tuples;
			}

			index_close(indexRelation);

			indexinfos = lcons(info, indexinfos);
		}

		list_free(indexoidlist);
	}

	rel->indexlist = indexinfos;

	/* close rel, but keep lock if any */
	heap_close(relation, NoLock);
}

/*
 * estimate_rel_size - estimate # pages and # tuples in a table or index
 *
 * If attr_widths isn't NULL, it points to the zero-index entry of the
 * relation's attr_width[] cache; we fill this in if we have need to compute
 * the attribute widths for estimation purposes.
 */
static void
estimate_rel_size(Relation rel, int32 *attr_widths,
				  BlockNumber *pages, double *tuples)
{
	BlockNumber curpages;
	BlockNumber relpages;
	double		reltuples;
	double		density;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_INDEX:
		case RELKIND_TOASTVALUE:
			/* it has storage, ok to call the smgr */
			curpages = RelationGetNumberOfBlocks(rel);

			/*
			 * HACK: if the relation has never yet been vacuumed, use a
			 * minimum estimate of 10 pages.  This emulates a desirable aspect
			 * of pre-8.0 behavior, which is that we wouldn't assume a newly
			 * created relation is really small, which saves us from making
			 * really bad plans during initial data loading.  (The plans are
			 * not wrong when they are made, but if they are cached and used
			 * again after the table has grown a lot, they are bad.) It would
			 * be better to force replanning if the table size has changed a
			 * lot since the plan was made ... but we don't currently have any
			 * infrastructure for redoing cached plans at all, so we have to
			 * kluge things here instead.
			 *
			 * We approximate "never vacuumed" by "has relpages = 0", which
			 * means this will also fire on genuinely empty relations.	Not
			 * great, but fortunately that's a seldom-seen case in the real
			 * world, and it shouldn't degrade the quality of the plan too
			 * much anyway to err in this direction.
			 */
			if (curpages < 10 && rel->rd_rel->relpages == 0)
				curpages = 10;

			/* report estimated # pages */
			*pages = curpages;
			/* quick exit if rel is clearly empty */
			if (curpages == 0)
			{
				*tuples = 0;
				break;
			}
			/* coerce values in pg_class to more desirable types */
			relpages = (BlockNumber) rel->rd_rel->relpages;
			reltuples = (double) rel->rd_rel->reltuples;

			/*
			 * If it's an index, discount the metapage.  This is a kluge
			 * because it assumes more than it ought to about index contents;
			 * it's reasonably OK for btrees but a bit suspect otherwise.
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
				int32		tuple_width = 0;
				int			i;

				for (i = 1; i <= RelationGetNumberOfAttributes(rel); i++)
				{
					Form_pg_attribute att = rel->rd_att->attrs[i - 1];
					int32		item_width;

					if (att->attisdropped)
						continue;
					/* This should match set_rel_width() in costsize.c */
					item_width = get_attavgwidth(RelationGetRelid(rel), i);
					if (item_width <= 0)
					{
						item_width = get_typavgwidth(att->atttypid,
													 att->atttypmod);
						Assert(item_width > 0);
					}
					if (attr_widths != NULL)
						attr_widths[i] = item_width;
					tuple_width += item_width;
				}
				tuple_width += sizeof(HeapTupleHeaderData);
				tuple_width += sizeof(ItemPointerData);
				/* note: integer division is intentional here */
				density = (BLCKSZ - sizeof(PageHeaderData)) / tuple_width;
			}
			*tuples = rint(density * (double) curpages);
			break;
		case RELKIND_SEQUENCE:
			/* Sequences always have a known size */
			*pages = 1;
			*tuples = 1;
			break;
		default:
			/* else it has no disk storage; probably shouldn't get here? */
			*pages = 0;
			*tuples = 0;
			break;
	}
}


/*
 * get_relation_constraints
 *
 * Retrieve the CHECK constraint expressions of the given relation.
 *
 * Returns a List (possibly empty) of constraint expressions.  Each one
 * has been canonicalized, and its Vars are changed to have the varno
 * indicated by rel->relid.  This allows the expressions to be easily
 * compared to expressions taken from WHERE.
 *
 * Note: at present this is invoked at most once per relation per planner
 * run, and in many cases it won't be invoked at all, so there seems no
 * point in caching the data in RelOptInfo.
 */
List *
get_relation_constraints(Oid relationObjectId, RelOptInfo *rel)
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
			cexpr = eval_const_expressions(cexpr);

			cexpr = (Node *) canonicalize_qual((Expr *) cexpr);

			/*
			 * Also mark any coercion format fields as "don't care", so that
			 * we can match to both explicit and implicit coercions.
			 */
			set_coercionform_dontcare(cexpr);

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
	}

	heap_close(relation, NoLock);

	return result;
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
 * We also support building a "physical" tlist for subqueries and functions,
 * since the same optimization can occur in SubqueryScan and FunctionScan
 * nodes.
 */
List *
build_physical_tlist(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *tlist = NIL;
	Index		varno = rel->relid;
	RangeTblEntry *rte = rt_fetch(varno, root->parse->rtable);
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
				var = makeVar(varno,
							  tle->resno,
							  exprType((Node *) tle->expr),
							  exprTypmod((Node *) tle->expr),
							  0);

				tlist = lappend(tlist,
								makeTargetEntry((Expr *) var,
												tle->resno,
												NULL,
												tle->resjunk));
			}
			break;

		case RTE_FUNCTION:
			expandRTE(rte, varno, 0, true /* include dropped */ ,
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
						Oid operator,
						List *args,
						int varRelid)
{
	RegProcedure oprrest = get_oprrest(operator);
	float8		result;

	/*
	 * if the oprrest procedure is missing for whatever reason, use a
	 * selectivity of 0.5
	 */
	if (!oprrest)
		return (Selectivity) 0.5;

	result = DatumGetFloat8(OidFunctionCall4(oprrest,
											 PointerGetDatum(root),
											 ObjectIdGetDatum(operator),
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
				 Oid operator,
				 List *args,
				 JoinType jointype)
{
	RegProcedure oprjoin = get_oprjoin(operator);
	float8		result;

	/*
	 * if the oprjoin procedure is missing for whatever reason, use a
	 * selectivity of 0.5
	 */
	if (!oprjoin)
		return (Selectivity) 0.5;

	result = DatumGetFloat8(OidFunctionCall4(oprjoin,
											 PointerGetDatum(root),
											 ObjectIdGetDatum(operator),
											 PointerGetDatum(args),
											 Int16GetDatum(jointype)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "invalid join selectivity: %f", result);

	return (Selectivity) result;
}

/*
 * find_inheritance_children
 *
 * Returns a list containing the OIDs of all relations which
 * inherit *directly* from the relation with OID 'inhparent'.
 *
 * XXX might be a good idea to create an index on pg_inherits' inhparent
 * field, so that we can use an indexscan instead of sequential scan here.
 * However, in typical databases pg_inherits won't have enough entries to
 * justify an indexscan...
 */
List *
find_inheritance_children(Oid inhparent)
{
	List	   *list = NIL;
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	inheritsTuple;
	Oid			inhrelid;
	ScanKeyData key[1];

	/*
	 * Can skip the scan if pg_class shows the relation has never had a
	 * subclass.
	 */
	if (!has_subclass(inhparent))
		return NIL;

	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(inhparent));
	relation = heap_open(InheritsRelationId, AccessShareLock);
	scan = heap_beginscan(relation, SnapshotNow, 1, key);
	while ((inheritsTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrelid;
		list = lappend_oid(list, inhrelid);
	}
	heap_endscan(scan);
	heap_close(relation, AccessShareLock);
	return list;
}

/*
 * has_subclass
 *
 * In the current implementation, has_subclass returns whether a
 * particular class *might* have a subclass. It will not return the
 * correct result if a class had a subclass which was later dropped.
 * This is because relhassubclass in pg_class is not updated when a
 * subclass is dropped, primarily because of concurrency concerns.
 *
 * Currently has_subclass is only used as an efficiency hack to skip
 * unnecessary inheritance searches, so this is OK.
 */
bool
has_subclass(Oid relationId)
{
	HeapTuple	tuple;
	bool		result;

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relationId),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationId);

	result = ((Form_pg_class) GETSTRUCT(tuple))->relhassubclass;
	ReleaseSysCache(tuple);
	return result;
}

/*
 * has_unique_index
 *
 * Detect whether there is a unique index on the specified attribute
 * of the specified relation, thus allowing us to conclude that all
 * the (non-null) values of the attribute are distinct.
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
		 * that all attr values are distinct.  We don't take any interest in
		 * expressional indexes either. Also, a multicolumn unique index
		 * doesn't allow us to conclude that just the specified attr is
		 * unique.
		 */
		if (index->unique &&
			index->ncolumns == 1 &&
			index->indexkeys[0] == attno &&
			index->indpred == NIL)
			return true;
	}
	return false;
}
