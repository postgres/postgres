/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/plancat.c,v 1.127.2.1 2008/01/12 00:11:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/pg_inherits.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/predtest.h"
#include "optimizer/prep.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "catalog/catalog.h"
#include "miscadmin.h"


/* GUC parameter */
bool		constraint_exclusion = false;


static void estimate_rel_size(Relation rel, int32 *attr_widths,
				  BlockNumber *pages, double *tuples);
static List *get_relation_constraints(Oid relationObjectId, RelOptInfo *rel,
						 bool include_notnull);


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

	rel->min_attr = FirstLowInvalidHeapAttributeNumber + 1;
	rel->max_attr = RelationGetNumberOfAttributes(relation);

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
						  &rel->pages, &rel->tuples);

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
			int16		amorderstrategy;

			/*
			 * Extract info from the relation descriptor for the index.
			 */
			indexRelation = index_open(indexoid, lmode);
			index = indexRelation->rd_index;

			/*
			 * Ignore invalid indexes, since they can't safely be used for
			 * queries.  Note that this is OK because the data structure we
			 * are constructing is only used by the planner --- the executor
			 * still needs to insert into "invalid" indexes!
			 */
			if (!index->indisvalid)
			{
				index_close(indexRelation, NoLock);
				continue;
			}

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

			index_close(indexRelation, NoLock);

			indexinfos = lcons(info, indexinfos);
		}

		list_free(indexoidlist);
	}

	rel->indexlist = indexinfos;

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
 * If include_notnull is true, "col IS NOT NULL" expressions are generated
 * and added to the result for each column that's marked attnotnull.
 *
 * Note: at present this is invoked at most once per relation per planner
 * run, and in many cases it won't be invoked at all, so there seems no
 * point in caching the data in RelOptInfo.
 */
static List *
get_relation_constraints(Oid relationObjectId, RelOptInfo *rel,
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

		/* Add NOT NULL constraints in expression form, if requested */
		if (include_notnull && constr->has_not_null)
		{
			int		natts = relation->rd_att->natts;

			for (i = 1; i <= natts; i++)
			{
				Form_pg_attribute att = relation->rd_att->attrs[i - 1];

				if (att->attnotnull && !att->attisdropped)
				{
					NullTest *ntest = makeNode(NullTest);

					ntest->arg = (Expr *) makeVar(varno,
												  i,
												  att->atttypid,
												  att->atttypmod,
												  0);
					ntest->nulltesttype = IS_NOT_NULL;
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
 * relation's CHECK constraints.
 */
bool
relation_excluded_by_constraints(RelOptInfo *rel, RangeTblEntry *rte)
{
	List	   *safe_restrictions;
	List	   *constraint_pred;
	List	   *safe_constraints;
	ListCell   *lc;

	/* Skip the test if constraint exclusion is disabled */
	if (!constraint_exclusion)
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
	 * OK to fetch the constraint expressions.  Include "col IS NOT NULL"
	 * expressions for attnotnull columns, in case we can refute those.
	 */
	constraint_pred = get_relation_constraints(rte->relid, rel, true);

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
 * and values lists, since the same optimization can occur in SubqueryScan,
 * FunctionScan, and ValuesScan nodes.
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

		case RTE_VALUES:
			expandRTE(rte, varno, 0, false /* dropped not applicable */ ,
					  NULL, &colvars);
			foreach(l, colvars)
			{
				var = (Var *) lfirst(l);

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
