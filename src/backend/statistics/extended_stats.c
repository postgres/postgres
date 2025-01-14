/*-------------------------------------------------------------------------
 *
 * extended_stats.c
 *	  POSTGRES extended statistics
 *
 * Generic code supporting statistics objects created via CREATE STATISTICS.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/extended_stats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "commands/defrem.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/attoptcache.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

/*
 * To avoid consuming too much memory during analysis and/or too much space
 * in the resulting pg_statistic rows, we ignore varlena datums that are wider
 * than WIDTH_THRESHOLD (after detoasting!).  This is legitimate for MCV
 * and distinct-value calculations since a wide value is unlikely to be
 * duplicated at all, much less be a most-common value.  For the same reason,
 * ignoring wide values will not affect our estimates of histogram bin
 * boundaries very much.
 */
#define WIDTH_THRESHOLD  1024

/*
 * Used internally to refer to an individual statistics object, i.e.,
 * a pg_statistic_ext entry.
 */
typedef struct StatExtEntry
{
	Oid			statOid;		/* OID of pg_statistic_ext entry */
	char	   *schema;			/* statistics object's schema */
	char	   *name;			/* statistics object's name */
	Bitmapset  *columns;		/* attribute numbers covered by the object */
	List	   *types;			/* 'char' list of enabled statistics kinds */
	int			stattarget;		/* statistics target (-1 for default) */
	List	   *exprs;			/* expressions */
} StatExtEntry;


static List *fetch_statentries_for_relation(Relation pg_statext, Oid relid);
static VacAttrStats **lookup_var_attr_stats(Bitmapset *attrs, List *exprs,
											int nvacatts, VacAttrStats **vacatts);
static void statext_store(Oid statOid, bool inh,
						  MVNDistinct *ndistinct, MVDependencies *dependencies,
						  MCVList *mcv, Datum exprs, VacAttrStats **stats);
static int	statext_compute_stattarget(int stattarget,
									   int nattrs, VacAttrStats **stats);

/* Information needed to analyze a single simple expression. */
typedef struct AnlExprData
{
	Node	   *expr;			/* expression to analyze */
	VacAttrStats *vacattrstat;	/* statistics attrs to analyze */
} AnlExprData;

static void compute_expr_stats(Relation onerel, AnlExprData *exprdata,
							   int nexprs, HeapTuple *rows, int numrows);
static Datum serialize_expr_stats(AnlExprData *exprdata, int nexprs);
static Datum expr_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull);
static AnlExprData *build_expr_data(List *exprs, int stattarget);

static StatsBuildData *make_build_data(Relation rel, StatExtEntry *stat,
									   int numrows, HeapTuple *rows,
									   VacAttrStats **stats, int stattarget);


/*
 * Compute requested extended stats, using the rows sampled for the plain
 * (single-column) stats.
 *
 * This fetches a list of stats types from pg_statistic_ext, computes the
 * requested stats, and serializes them back into the catalog.
 */
void
BuildRelationExtStatistics(Relation onerel, bool inh, double totalrows,
						   int numrows, HeapTuple *rows,
						   int natts, VacAttrStats **vacattrstats)
{
	Relation	pg_stext;
	ListCell   *lc;
	List	   *statslist;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int64		ext_cnt;

	/* Do nothing if there are no columns to analyze. */
	if (!natts)
		return;

	/* the list of stats has to be allocated outside the memory context */
	pg_stext = table_open(StatisticExtRelationId, RowExclusiveLock);
	statslist = fetch_statentries_for_relation(pg_stext, RelationGetRelid(onerel));

	/* memory context for building each statistics object */
	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"BuildRelationExtStatistics",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	/* report this phase */
	if (statslist != NIL)
	{
		const int	index[] = {
			PROGRESS_ANALYZE_PHASE,
			PROGRESS_ANALYZE_EXT_STATS_TOTAL
		};
		const int64 val[] = {
			PROGRESS_ANALYZE_PHASE_COMPUTE_EXT_STATS,
			list_length(statslist)
		};

		pgstat_progress_update_multi_param(2, index, val);
	}

	ext_cnt = 0;
	foreach(lc, statslist)
	{
		StatExtEntry *stat = (StatExtEntry *) lfirst(lc);
		MVNDistinct *ndistinct = NULL;
		MVDependencies *dependencies = NULL;
		MCVList    *mcv = NULL;
		Datum		exprstats = (Datum) 0;
		VacAttrStats **stats;
		ListCell   *lc2;
		int			stattarget;
		StatsBuildData *data;

		/*
		 * Check if we can build these stats based on the column analyzed. If
		 * not, report this fact (except in autovacuum) and move on.
		 */
		stats = lookup_var_attr_stats(stat->columns, stat->exprs,
									  natts, vacattrstats);
		if (!stats)
		{
			if (!AmAutoVacuumWorkerProcess())
				ereport(WARNING,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("statistics object \"%s.%s\" could not be computed for relation \"%s.%s\"",
								stat->schema, stat->name,
								get_namespace_name(onerel->rd_rel->relnamespace),
								RelationGetRelationName(onerel)),
						 errtable(onerel)));
			continue;
		}

		/* compute statistics target for this statistics object */
		stattarget = statext_compute_stattarget(stat->stattarget,
												bms_num_members(stat->columns),
												stats);

		/*
		 * Don't rebuild statistics objects with statistics target set to 0
		 * (we just leave the existing values around, just like we do for
		 * regular per-column statistics).
		 */
		if (stattarget == 0)
			continue;

		/* evaluate expressions (if the statistics object has any) */
		data = make_build_data(onerel, stat, numrows, rows, stats, stattarget);

		/* compute statistic of each requested type */
		foreach(lc2, stat->types)
		{
			char		t = (char) lfirst_int(lc2);

			if (t == STATS_EXT_NDISTINCT)
				ndistinct = statext_ndistinct_build(totalrows, data);
			else if (t == STATS_EXT_DEPENDENCIES)
				dependencies = statext_dependencies_build(data);
			else if (t == STATS_EXT_MCV)
				mcv = statext_mcv_build(data, totalrows, stattarget);
			else if (t == STATS_EXT_EXPRESSIONS)
			{
				AnlExprData *exprdata;
				int			nexprs;

				/* should not happen, thanks to checks when defining stats */
				if (!stat->exprs)
					elog(ERROR, "requested expression stats, but there are no expressions");

				exprdata = build_expr_data(stat->exprs, stattarget);
				nexprs = list_length(stat->exprs);

				compute_expr_stats(onerel, exprdata, nexprs, rows, numrows);

				exprstats = serialize_expr_stats(exprdata, nexprs);
			}
		}

		/* store the statistics in the catalog */
		statext_store(stat->statOid, inh,
					  ndistinct, dependencies, mcv, exprstats, stats);

		/* for reporting progress */
		pgstat_progress_update_param(PROGRESS_ANALYZE_EXT_STATS_COMPUTED,
									 ++ext_cnt);

		/* free the data used for building this statistics object */
		MemoryContextReset(cxt);
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	list_free(statslist);

	table_close(pg_stext, RowExclusiveLock);
}

/*
 * ComputeExtStatisticsRows
 *		Compute number of rows required by extended statistics on a table.
 *
 * Computes number of rows we need to sample to build extended statistics on a
 * table. This only looks at statistics we can actually build - for example
 * when analyzing only some of the columns, this will skip statistics objects
 * that would require additional columns.
 *
 * See statext_compute_stattarget for details about how we compute the
 * statistics target for a statistics object (from the object target,
 * attribute targets and default statistics target).
 */
int
ComputeExtStatisticsRows(Relation onerel,
						 int natts, VacAttrStats **vacattrstats)
{
	Relation	pg_stext;
	ListCell   *lc;
	List	   *lstats;
	MemoryContext cxt;
	MemoryContext oldcxt;
	int			result = 0;

	/* If there are no columns to analyze, just return 0. */
	if (!natts)
		return 0;

	cxt = AllocSetContextCreate(CurrentMemoryContext,
								"ComputeExtStatisticsRows",
								ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(cxt);

	pg_stext = table_open(StatisticExtRelationId, RowExclusiveLock);
	lstats = fetch_statentries_for_relation(pg_stext, RelationGetRelid(onerel));

	foreach(lc, lstats)
	{
		StatExtEntry *stat = (StatExtEntry *) lfirst(lc);
		int			stattarget;
		VacAttrStats **stats;
		int			nattrs = bms_num_members(stat->columns);

		/*
		 * Check if we can build this statistics object based on the columns
		 * analyzed. If not, ignore it (don't report anything, we'll do that
		 * during the actual build BuildRelationExtStatistics).
		 */
		stats = lookup_var_attr_stats(stat->columns, stat->exprs,
									  natts, vacattrstats);

		if (!stats)
			continue;

		/*
		 * Compute statistics target, based on what's set for the statistic
		 * object itself, and for its attributes.
		 */
		stattarget = statext_compute_stattarget(stat->stattarget,
												nattrs, stats);

		/* Use the largest value for all statistics objects. */
		if (stattarget > result)
			result = stattarget;
	}

	table_close(pg_stext, RowExclusiveLock);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);

	/* compute sample size based on the statistics target */
	return (300 * result);
}

/*
 * statext_compute_stattarget
 *		compute statistics target for an extended statistic
 *
 * When computing target for extended statistics objects, we consider three
 * places where the target may be set - the statistics object itself,
 * attributes the statistics object is defined on, and then the default
 * statistics target.
 *
 * First we look at what's set for the statistics object itself, using the
 * ALTER STATISTICS ... SET STATISTICS command. If we find a valid value
 * there (i.e. not -1) we're done. Otherwise we look at targets set for any
 * of the attributes the statistic is defined on, and if there are columns
 * with defined target, we use the maximum value. We do this mostly for
 * backwards compatibility, because this is what we did before having
 * statistics target for extended statistics.
 *
 * And finally, if we still don't have a statistics target, we use the value
 * set in default_statistics_target.
 */
static int
statext_compute_stattarget(int stattarget, int nattrs, VacAttrStats **stats)
{
	int			i;

	/*
	 * If there's statistics target set for the statistics object, use it. It
	 * may be set to 0 which disables building of that statistic.
	 */
	if (stattarget >= 0)
		return stattarget;

	/*
	 * The target for the statistics object is set to -1, in which case we
	 * look at the maximum target set for any of the attributes the object is
	 * defined on.
	 */
	for (i = 0; i < nattrs; i++)
	{
		/* keep the maximum statistics target */
		if (stats[i]->attstattarget > stattarget)
			stattarget = stats[i]->attstattarget;
	}

	/*
	 * If the value is still negative (so neither the statistics object nor
	 * any of the columns have custom statistics target set), use the global
	 * default target.
	 */
	if (stattarget < 0)
		stattarget = default_statistics_target;

	/* As this point we should have a valid statistics target. */
	Assert((stattarget >= 0) && (stattarget <= MAX_STATISTICS_TARGET));

	return stattarget;
}

/*
 * statext_is_kind_built
 *		Is this stat kind built in the given pg_statistic_ext_data tuple?
 */
bool
statext_is_kind_built(HeapTuple htup, char type)
{
	AttrNumber	attnum;

	switch (type)
	{
		case STATS_EXT_NDISTINCT:
			attnum = Anum_pg_statistic_ext_data_stxdndistinct;
			break;

		case STATS_EXT_DEPENDENCIES:
			attnum = Anum_pg_statistic_ext_data_stxddependencies;
			break;

		case STATS_EXT_MCV:
			attnum = Anum_pg_statistic_ext_data_stxdmcv;
			break;

		case STATS_EXT_EXPRESSIONS:
			attnum = Anum_pg_statistic_ext_data_stxdexpr;
			break;

		default:
			elog(ERROR, "unexpected statistics type requested: %d", type);
	}

	return !heap_attisnull(htup, attnum, NULL);
}

/*
 * Return a list (of StatExtEntry) of statistics objects for the given relation.
 */
static List *
fetch_statentries_for_relation(Relation pg_statext, Oid relid)
{
	SysScanDesc scan;
	ScanKeyData skey;
	HeapTuple	htup;
	List	   *result = NIL;

	/*
	 * Prepare to scan pg_statistic_ext for entries having stxrelid = this
	 * rel.
	 */
	ScanKeyInit(&skey,
				Anum_pg_statistic_ext_stxrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(pg_statext, StatisticExtRelidIndexId, true,
							  NULL, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		StatExtEntry *entry;
		Datum		datum;
		bool		isnull;
		int			i;
		ArrayType  *arr;
		char	   *enabled;
		Form_pg_statistic_ext staForm;
		List	   *exprs = NIL;

		entry = palloc0(sizeof(StatExtEntry));
		staForm = (Form_pg_statistic_ext) GETSTRUCT(htup);
		entry->statOid = staForm->oid;
		entry->schema = get_namespace_name(staForm->stxnamespace);
		entry->name = pstrdup(NameStr(staForm->stxname));
		for (i = 0; i < staForm->stxkeys.dim1; i++)
		{
			entry->columns = bms_add_member(entry->columns,
											staForm->stxkeys.values[i]);
		}

		datum = SysCacheGetAttr(STATEXTOID, htup, Anum_pg_statistic_ext_stxstattarget, &isnull);
		entry->stattarget = isnull ? -1 : DatumGetInt16(datum);

		/* decode the stxkind char array into a list of chars */
		datum = SysCacheGetAttrNotNull(STATEXTOID, htup,
									   Anum_pg_statistic_ext_stxkind);
		arr = DatumGetArrayTypeP(datum);
		if (ARR_NDIM(arr) != 1 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "stxkind is not a 1-D char array");
		enabled = (char *) ARR_DATA_PTR(arr);
		for (i = 0; i < ARR_DIMS(arr)[0]; i++)
		{
			Assert((enabled[i] == STATS_EXT_NDISTINCT) ||
				   (enabled[i] == STATS_EXT_DEPENDENCIES) ||
				   (enabled[i] == STATS_EXT_MCV) ||
				   (enabled[i] == STATS_EXT_EXPRESSIONS));
			entry->types = lappend_int(entry->types, (int) enabled[i]);
		}

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
			 * Run the expressions through eval_const_expressions. This is not
			 * just an optimization, but is necessary, because the planner
			 * will be comparing them to similarly-processed qual clauses, and
			 * may fail to detect valid matches without this.  We must not use
			 * canonicalize_qual, however, since these aren't qual
			 * expressions.
			 */
			exprs = (List *) eval_const_expressions(NULL, (Node *) exprs);

			/* May as well fix opfuncids too */
			fix_opfuncids((Node *) exprs);
		}

		entry->exprs = exprs;

		result = lappend(result, entry);
	}

	systable_endscan(scan);

	return result;
}

/*
 * examine_attribute -- pre-analysis of a single column
 *
 * Determine whether the column is analyzable; if so, create and initialize
 * a VacAttrStats struct for it.  If not, return NULL.
 */
static VacAttrStats *
examine_attribute(Node *expr)
{
	HeapTuple	typtuple;
	VacAttrStats *stats;
	int			i;
	bool		ok;

	/*
	 * Create the VacAttrStats struct.
	 */
	stats = (VacAttrStats *) palloc0(sizeof(VacAttrStats));
	stats->attstattarget = -1;

	/*
	 * When analyzing an expression, believe the expression tree's type not
	 * the column datatype --- the latter might be the opckeytype storage type
	 * of the opclass, which is not interesting for our purposes.  (Note: if
	 * we did anything with non-expression statistics columns, we'd need to
	 * figure out where to get the correct type info from, but for now that's
	 * not a problem.)	It's not clear whether anyone will care about the
	 * typmod, but we store that too just in case.
	 */
	stats->attrtypid = exprType(expr);
	stats->attrtypmod = exprTypmod(expr);
	stats->attrcollid = exprCollation(expr);

	typtuple = SearchSysCacheCopy1(TYPEOID,
								   ObjectIdGetDatum(stats->attrtypid));
	if (!HeapTupleIsValid(typtuple))
		elog(ERROR, "cache lookup failed for type %u", stats->attrtypid);
	stats->attrtype = (Form_pg_type) GETSTRUCT(typtuple);

	/*
	 * We don't actually analyze individual attributes, so no need to set the
	 * memory context.
	 */
	stats->anl_context = NULL;
	stats->tupattnum = InvalidAttrNumber;

	/*
	 * The fields describing the stats->stavalues[n] element types default to
	 * the type of the data being analyzed, but the type-specific typanalyze
	 * function can change them if it wants to store something else.
	 */
	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		stats->statypid[i] = stats->attrtypid;
		stats->statyplen[i] = stats->attrtype->typlen;
		stats->statypbyval[i] = stats->attrtype->typbyval;
		stats->statypalign[i] = stats->attrtype->typalign;
	}

	/*
	 * Call the type-specific typanalyze function.  If none is specified, use
	 * std_typanalyze().
	 */
	if (OidIsValid(stats->attrtype->typanalyze))
		ok = DatumGetBool(OidFunctionCall1(stats->attrtype->typanalyze,
										   PointerGetDatum(stats)));
	else
		ok = std_typanalyze(stats);

	if (!ok || stats->compute_stats == NULL || stats->minrows <= 0)
	{
		heap_freetuple(typtuple);
		pfree(stats);
		return NULL;
	}

	return stats;
}

/*
 * examine_expression -- pre-analysis of a single expression
 *
 * Determine whether the expression is analyzable; if so, create and initialize
 * a VacAttrStats struct for it.  If not, return NULL.
 */
static VacAttrStats *
examine_expression(Node *expr, int stattarget)
{
	HeapTuple	typtuple;
	VacAttrStats *stats;
	int			i;
	bool		ok;

	Assert(expr != NULL);

	/*
	 * Create the VacAttrStats struct.
	 */
	stats = (VacAttrStats *) palloc0(sizeof(VacAttrStats));

	/*
	 * We can't have statistics target specified for the expression, so we
	 * could use either the default_statistics_target, or the target computed
	 * for the extended statistics. The second option seems more reasonable.
	 */
	stats->attstattarget = stattarget;

	/*
	 * When analyzing an expression, believe the expression tree's type.
	 */
	stats->attrtypid = exprType(expr);
	stats->attrtypmod = exprTypmod(expr);

	/*
	 * We don't allow collation to be specified in CREATE STATISTICS, so we
	 * have to use the collation specified for the expression. It's possible
	 * to specify the collation in the expression "(col COLLATE "en_US")" in
	 * which case exprCollation() does the right thing.
	 */
	stats->attrcollid = exprCollation(expr);

	typtuple = SearchSysCacheCopy1(TYPEOID,
								   ObjectIdGetDatum(stats->attrtypid));
	if (!HeapTupleIsValid(typtuple))
		elog(ERROR, "cache lookup failed for type %u", stats->attrtypid);

	stats->attrtype = (Form_pg_type) GETSTRUCT(typtuple);
	stats->anl_context = CurrentMemoryContext;	/* XXX should be using
												 * something else? */
	stats->tupattnum = InvalidAttrNumber;

	/*
	 * The fields describing the stats->stavalues[n] element types default to
	 * the type of the data being analyzed, but the type-specific typanalyze
	 * function can change them if it wants to store something else.
	 */
	for (i = 0; i < STATISTIC_NUM_SLOTS; i++)
	{
		stats->statypid[i] = stats->attrtypid;
		stats->statyplen[i] = stats->attrtype->typlen;
		stats->statypbyval[i] = stats->attrtype->typbyval;
		stats->statypalign[i] = stats->attrtype->typalign;
	}

	/*
	 * Call the type-specific typanalyze function.  If none is specified, use
	 * std_typanalyze().
	 */
	if (OidIsValid(stats->attrtype->typanalyze))
		ok = DatumGetBool(OidFunctionCall1(stats->attrtype->typanalyze,
										   PointerGetDatum(stats)));
	else
		ok = std_typanalyze(stats);

	if (!ok || stats->compute_stats == NULL || stats->minrows <= 0)
	{
		heap_freetuple(typtuple);
		pfree(stats);
		return NULL;
	}

	return stats;
}

/*
 * Using 'vacatts' of size 'nvacatts' as input data, return a newly-built
 * VacAttrStats array which includes only the items corresponding to
 * attributes indicated by 'attrs'.  If we don't have all of the per-column
 * stats available to compute the extended stats, then we return NULL to
 * indicate to the caller that the stats should not be built.
 */
static VacAttrStats **
lookup_var_attr_stats(Bitmapset *attrs, List *exprs,
					  int nvacatts, VacAttrStats **vacatts)
{
	int			i = 0;
	int			x = -1;
	int			natts;
	VacAttrStats **stats;
	ListCell   *lc;

	natts = bms_num_members(attrs) + list_length(exprs);

	stats = (VacAttrStats **) palloc(natts * sizeof(VacAttrStats *));

	/* lookup VacAttrStats info for the requested columns (same attnum) */
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		int			j;

		stats[i] = NULL;
		for (j = 0; j < nvacatts; j++)
		{
			if (x == vacatts[j]->tupattnum)
			{
				stats[i] = vacatts[j];
				break;
			}
		}

		if (!stats[i])
		{
			/*
			 * Looks like stats were not gathered for one of the columns
			 * required. We'll be unable to build the extended stats without
			 * this column.
			 */
			pfree(stats);
			return NULL;
		}

		i++;
	}

	/* also add info for expressions */
	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		stats[i] = examine_attribute(expr);

		/*
		 * XXX We need tuple descriptor later, and we just grab it from
		 * stats[0]->tupDesc (see e.g. statext_mcv_build). But as coded
		 * examine_attribute does not set that, so just grab it from the first
		 * vacatts element.
		 */
		stats[i]->tupDesc = vacatts[0]->tupDesc;

		i++;
	}

	return stats;
}

/*
 * statext_store
 *	Serializes the statistics and stores them into the pg_statistic_ext_data
 *	tuple.
 */
static void
statext_store(Oid statOid, bool inh,
			  MVNDistinct *ndistinct, MVDependencies *dependencies,
			  MCVList *mcv, Datum exprs, VacAttrStats **stats)
{
	Relation	pg_stextdata;
	HeapTuple	stup;
	Datum		values[Natts_pg_statistic_ext_data];
	bool		nulls[Natts_pg_statistic_ext_data];

	pg_stextdata = table_open(StatisticExtDataRelationId, RowExclusiveLock);

	memset(nulls, true, sizeof(nulls));
	memset(values, 0, sizeof(values));

	/* basic info */
	values[Anum_pg_statistic_ext_data_stxoid - 1] = ObjectIdGetDatum(statOid);
	nulls[Anum_pg_statistic_ext_data_stxoid - 1] = false;

	values[Anum_pg_statistic_ext_data_stxdinherit - 1] = BoolGetDatum(inh);
	nulls[Anum_pg_statistic_ext_data_stxdinherit - 1] = false;

	/*
	 * Construct a new pg_statistic_ext_data tuple, replacing the calculated
	 * stats.
	 */
	if (ndistinct != NULL)
	{
		bytea	   *data = statext_ndistinct_serialize(ndistinct);

		nulls[Anum_pg_statistic_ext_data_stxdndistinct - 1] = (data == NULL);
		values[Anum_pg_statistic_ext_data_stxdndistinct - 1] = PointerGetDatum(data);
	}

	if (dependencies != NULL)
	{
		bytea	   *data = statext_dependencies_serialize(dependencies);

		nulls[Anum_pg_statistic_ext_data_stxddependencies - 1] = (data == NULL);
		values[Anum_pg_statistic_ext_data_stxddependencies - 1] = PointerGetDatum(data);
	}
	if (mcv != NULL)
	{
		bytea	   *data = statext_mcv_serialize(mcv, stats);

		nulls[Anum_pg_statistic_ext_data_stxdmcv - 1] = (data == NULL);
		values[Anum_pg_statistic_ext_data_stxdmcv - 1] = PointerGetDatum(data);
	}
	if (exprs != (Datum) 0)
	{
		nulls[Anum_pg_statistic_ext_data_stxdexpr - 1] = false;
		values[Anum_pg_statistic_ext_data_stxdexpr - 1] = exprs;
	}

	/*
	 * Delete the old tuple if it exists, and insert a new one. It's easier
	 * than trying to update or insert, based on various conditions.
	 */
	RemoveStatisticsDataById(statOid, inh);

	/* form and insert a new tuple */
	stup = heap_form_tuple(RelationGetDescr(pg_stextdata), values, nulls);
	CatalogTupleInsert(pg_stextdata, stup);

	heap_freetuple(stup);

	table_close(pg_stextdata, RowExclusiveLock);
}

/* initialize multi-dimensional sort */
MultiSortSupport
multi_sort_init(int ndims)
{
	MultiSortSupport mss;

	Assert(ndims >= 2);

	mss = (MultiSortSupport) palloc0(offsetof(MultiSortSupportData, ssup)
									 + sizeof(SortSupportData) * ndims);

	mss->ndims = ndims;

	return mss;
}

/*
 * Prepare sort support info using the given sort operator and collation
 * at the position 'sortdim'
 */
void
multi_sort_add_dimension(MultiSortSupport mss, int sortdim,
						 Oid oper, Oid collation)
{
	SortSupport ssup = &mss->ssup[sortdim];

	ssup->ssup_cxt = CurrentMemoryContext;
	ssup->ssup_collation = collation;
	ssup->ssup_nulls_first = false;

	PrepareSortSupportFromOrderingOp(oper, ssup);
}

/* compare all the dimensions in the selected order */
int
multi_sort_compare(const void *a, const void *b, void *arg)
{
	MultiSortSupport mss = (MultiSortSupport) arg;
	SortItem   *ia = (SortItem *) a;
	SortItem   *ib = (SortItem *) b;
	int			i;

	for (i = 0; i < mss->ndims; i++)
	{
		int			compare;

		compare = ApplySortComparator(ia->values[i], ia->isnull[i],
									  ib->values[i], ib->isnull[i],
									  &mss->ssup[i]);

		if (compare != 0)
			return compare;
	}

	/* equal by default */
	return 0;
}

/* compare selected dimension */
int
multi_sort_compare_dim(int dim, const SortItem *a, const SortItem *b,
					   MultiSortSupport mss)
{
	return ApplySortComparator(a->values[dim], a->isnull[dim],
							   b->values[dim], b->isnull[dim],
							   &mss->ssup[dim]);
}

int
multi_sort_compare_dims(int start, int end,
						const SortItem *a, const SortItem *b,
						MultiSortSupport mss)
{
	int			dim;

	for (dim = start; dim <= end; dim++)
	{
		int			r = ApplySortComparator(a->values[dim], a->isnull[dim],
											b->values[dim], b->isnull[dim],
											&mss->ssup[dim]);

		if (r != 0)
			return r;
	}

	return 0;
}

int
compare_scalars_simple(const void *a, const void *b, void *arg)
{
	return compare_datums_simple(*(Datum *) a,
								 *(Datum *) b,
								 (SortSupport) arg);
}

int
compare_datums_simple(Datum a, Datum b, SortSupport ssup)
{
	return ApplySortComparator(a, false, b, false, ssup);
}

/*
 * build_attnums_array
 *		Transforms a bitmap into an array of AttrNumber values.
 *
 * This is used for extended statistics only, so all the attributes must be
 * user-defined. That means offsetting by FirstLowInvalidHeapAttributeNumber
 * is not necessary here (and when querying the bitmap).
 */
AttrNumber *
build_attnums_array(Bitmapset *attrs, int nexprs, int *numattrs)
{
	int			i,
				j;
	AttrNumber *attnums;
	int			num = bms_num_members(attrs);

	if (numattrs)
		*numattrs = num;

	/* build attnums from the bitmapset */
	attnums = (AttrNumber *) palloc(sizeof(AttrNumber) * num);
	i = 0;
	j = -1;
	while ((j = bms_next_member(attrs, j)) >= 0)
	{
		int			attnum = (j - nexprs);

		/*
		 * Make sure the bitmap contains only user-defined attributes. As
		 * bitmaps can't contain negative values, this can be violated in two
		 * ways. Firstly, the bitmap might contain 0 as a member, and secondly
		 * the integer value might be larger than MaxAttrNumber.
		 */
		Assert(AttributeNumberIsValid(attnum));
		Assert(attnum <= MaxAttrNumber);
		Assert(attnum >= (-nexprs));

		attnums[i++] = (AttrNumber) attnum;

		/* protect against overflows */
		Assert(i <= num);
	}

	return attnums;
}

/*
 * build_sorted_items
 *		build a sorted array of SortItem with values from rows
 *
 * Note: All the memory is allocated in a single chunk, so that the caller
 * can simply pfree the return value to release all of it.
 */
SortItem *
build_sorted_items(StatsBuildData *data, int *nitems,
				   MultiSortSupport mss,
				   int numattrs, AttrNumber *attnums)
{
	int			i,
				j,
				len,
				nrows;
	int			nvalues = data->numrows * numattrs;

	SortItem   *items;
	Datum	   *values;
	bool	   *isnull;
	char	   *ptr;
	int		   *typlen;

	/* Compute the total amount of memory we need (both items and values). */
	len = data->numrows * sizeof(SortItem) + nvalues * (sizeof(Datum) + sizeof(bool));

	/* Allocate the memory and split it into the pieces. */
	ptr = palloc0(len);

	/* items to sort */
	items = (SortItem *) ptr;
	ptr += data->numrows * sizeof(SortItem);

	/* values and null flags */
	values = (Datum *) ptr;
	ptr += nvalues * sizeof(Datum);

	isnull = (bool *) ptr;
	ptr += nvalues * sizeof(bool);

	/* make sure we consumed the whole buffer exactly */
	Assert((ptr - (char *) items) == len);

	/* fix the pointers to Datum and bool arrays */
	nrows = 0;
	for (i = 0; i < data->numrows; i++)
	{
		items[nrows].values = &values[nrows * numattrs];
		items[nrows].isnull = &isnull[nrows * numattrs];

		nrows++;
	}

	/* build a local cache of typlen for all attributes */
	typlen = (int *) palloc(sizeof(int) * data->nattnums);
	for (i = 0; i < data->nattnums; i++)
		typlen[i] = get_typlen(data->stats[i]->attrtypid);

	nrows = 0;
	for (i = 0; i < data->numrows; i++)
	{
		bool		toowide = false;

		/* load the values/null flags from sample rows */
		for (j = 0; j < numattrs; j++)
		{
			Datum		value;
			bool		isnull;
			int			attlen;
			AttrNumber	attnum = attnums[j];

			int			idx;

			/* match attnum to the pre-calculated data */
			for (idx = 0; idx < data->nattnums; idx++)
			{
				if (attnum == data->attnums[idx])
					break;
			}

			Assert(idx < data->nattnums);

			value = data->values[idx][i];
			isnull = data->nulls[idx][i];
			attlen = typlen[idx];

			/*
			 * If this is a varlena value, check if it's too wide and if yes
			 * then skip the whole item. Otherwise detoast the value.
			 *
			 * XXX It may happen that we've already detoasted some preceding
			 * values for the current item. We don't bother to cleanup those
			 * on the assumption that those are small (below WIDTH_THRESHOLD)
			 * and will be discarded at the end of analyze.
			 */
			if ((!isnull) && (attlen == -1))
			{
				if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
				{
					toowide = true;
					break;
				}

				value = PointerGetDatum(PG_DETOAST_DATUM(value));
			}

			items[nrows].values[j] = value;
			items[nrows].isnull[j] = isnull;
		}

		if (toowide)
			continue;

		nrows++;
	}

	/* store the actual number of items (ignoring the too-wide ones) */
	*nitems = nrows;

	/* all items were too wide */
	if (nrows == 0)
	{
		/* everything is allocated as a single chunk */
		pfree(items);
		return NULL;
	}

	/* do the sort, using the multi-sort */
	qsort_interruptible(items, nrows, sizeof(SortItem),
						multi_sort_compare, mss);

	return items;
}

/*
 * has_stats_of_kind
 *		Check whether the list contains statistic of a given kind
 */
bool
has_stats_of_kind(List *stats, char requiredkind)
{
	ListCell   *l;

	foreach(l, stats)
	{
		StatisticExtInfo *stat = (StatisticExtInfo *) lfirst(l);

		if (stat->kind == requiredkind)
			return true;
	}

	return false;
}

/*
 * stat_find_expression
 *		Search for an expression in statistics object's list of expressions.
 *
 * Returns the index of the expression in the statistics object's list of
 * expressions, or -1 if not found.
 */
static int
stat_find_expression(StatisticExtInfo *stat, Node *expr)
{
	ListCell   *lc;
	int			idx;

	idx = 0;
	foreach(lc, stat->exprs)
	{
		Node	   *stat_expr = (Node *) lfirst(lc);

		if (equal(stat_expr, expr))
			return idx;
		idx++;
	}

	/* Expression not found */
	return -1;
}

/*
 * stat_covers_expressions
 * 		Test whether a statistics object covers all expressions in a list.
 *
 * Returns true if all expressions are covered.  If expr_idxs is non-NULL, it
 * is populated with the indexes of the expressions found.
 */
static bool
stat_covers_expressions(StatisticExtInfo *stat, List *exprs,
						Bitmapset **expr_idxs)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		int			expr_idx;

		expr_idx = stat_find_expression(stat, expr);
		if (expr_idx == -1)
			return false;

		if (expr_idxs != NULL)
			*expr_idxs = bms_add_member(*expr_idxs, expr_idx);
	}

	/* If we reach here, all expressions are covered */
	return true;
}

/*
 * choose_best_statistics
 *		Look for and return statistics with the specified 'requiredkind' which
 *		have keys that match at least two of the given attnums.  Return NULL if
 *		there's no match.
 *
 * The current selection criteria is very simple - we choose the statistics
 * object referencing the most attributes in covered (and still unestimated
 * clauses), breaking ties in favor of objects with fewer keys overall.
 *
 * The clause_attnums is an array of bitmaps, storing attnums for individual
 * clauses. A NULL element means the clause is either incompatible or already
 * estimated.
 *
 * XXX If multiple statistics objects tie on both criteria, then which object
 * is chosen depends on the order that they appear in the stats list. Perhaps
 * further tiebreakers are needed.
 */
StatisticExtInfo *
choose_best_statistics(List *stats, char requiredkind, bool inh,
					   Bitmapset **clause_attnums, List **clause_exprs,
					   int nclauses)
{
	ListCell   *lc;
	StatisticExtInfo *best_match = NULL;
	int			best_num_matched = 2;	/* goal #1: maximize */
	int			best_match_keys = (STATS_MAX_DIMENSIONS + 1);	/* goal #2: minimize */

	foreach(lc, stats)
	{
		int			i;
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);
		Bitmapset  *matched_attnums = NULL;
		Bitmapset  *matched_exprs = NULL;
		int			num_matched;
		int			numkeys;

		/* skip statistics that are not of the correct type */
		if (info->kind != requiredkind)
			continue;

		/* skip statistics with mismatching inheritance flag */
		if (info->inherit != inh)
			continue;

		/*
		 * Collect attributes and expressions in remaining (unestimated)
		 * clauses fully covered by this statistic object.
		 *
		 * We know already estimated clauses have both clause_attnums and
		 * clause_exprs set to NULL. We leave the pointers NULL if already
		 * estimated, or we reset them to NULL after estimating the clause.
		 */
		for (i = 0; i < nclauses; i++)
		{
			Bitmapset  *expr_idxs = NULL;

			/* ignore incompatible/estimated clauses */
			if (!clause_attnums[i] && !clause_exprs[i])
				continue;

			/* ignore clauses that are not covered by this object */
			if (!bms_is_subset(clause_attnums[i], info->keys) ||
				!stat_covers_expressions(info, clause_exprs[i], &expr_idxs))
				continue;

			/* record attnums and indexes of expressions covered */
			matched_attnums = bms_add_members(matched_attnums, clause_attnums[i]);
			matched_exprs = bms_add_members(matched_exprs, expr_idxs);
		}

		num_matched = bms_num_members(matched_attnums) + bms_num_members(matched_exprs);

		bms_free(matched_attnums);
		bms_free(matched_exprs);

		/*
		 * save the actual number of keys in the stats so that we can choose
		 * the narrowest stats with the most matching keys.
		 */
		numkeys = bms_num_members(info->keys) + list_length(info->exprs);

		/*
		 * Use this object when it increases the number of matched attributes
		 * and expressions or when it matches the same number of attributes
		 * and expressions but these stats have fewer keys than any previous
		 * match.
		 */
		if (num_matched > best_num_matched ||
			(num_matched == best_num_matched && numkeys < best_match_keys))
		{
			best_match = info;
			best_num_matched = num_matched;
			best_match_keys = numkeys;
		}
	}

	return best_match;
}

/*
 * statext_is_compatible_clause_internal
 *		Determines if the clause is compatible with MCV lists.
 *
 * To be compatible, the given clause must be a combination of supported
 * clauses built from Vars or sub-expressions (where a sub-expression is
 * something that exactly matches an expression found in statistics objects).
 * This function recursively examines the clause and extracts any
 * sub-expressions that will need to be matched against statistics.
 *
 * Currently, we only support the following types of clauses:
 *
 * (a) OpExprs of the form (Var/Expr op Const), or (Const op Var/Expr), where
 * the op is one of ("=", "<", ">", ">=", "<=")
 *
 * (b) (Var/Expr IS [NOT] NULL)
 *
 * (c) combinations using AND/OR/NOT
 *
 * (d) ScalarArrayOpExprs of the form (Var/Expr op ANY (Const)) or
 * (Var/Expr op ALL (Const))
 *
 * In the future, the range of supported clauses may be expanded to more
 * complex cases, for example (Var op Var).
 *
 * Arguments:
 * clause: (sub)clause to be inspected (bare clause, not a RestrictInfo)
 * relid: rel that all Vars in clause must belong to
 * *attnums: input/output parameter collecting attribute numbers of all
 *		mentioned Vars.  Note that we do not offset the attribute numbers,
 *		so we can't cope with system columns.
 * *exprs: input/output parameter collecting primitive subclauses within
 *		the clause tree
 *
 * Returns false if there is something we definitively can't handle.
 * On true return, we can proceed to match the *exprs against statistics.
 */
static bool
statext_is_compatible_clause_internal(PlannerInfo *root, Node *clause,
									  Index relid, Bitmapset **attnums,
									  List **exprs)
{
	/* Look inside any binary-compatible relabeling (as in examine_variable) */
	if (IsA(clause, RelabelType))
		clause = (Node *) ((RelabelType *) clause)->arg;

	/* plain Var references (boolean Vars or recursive checks) */
	if (IsA(clause, Var))
	{
		Var		   *var = (Var *) clause;

		/* Ensure var is from the correct relation */
		if (var->varno != relid)
			return false;

		/* we also better ensure the Var is from the current level */
		if (var->varlevelsup > 0)
			return false;

		/*
		 * Also reject system attributes and whole-row Vars (we don't allow
		 * stats on those).
		 */
		if (!AttrNumberIsForUserDefinedAttr(var->varattno))
			return false;

		/* OK, record the attnum for later permissions checks. */
		*attnums = bms_add_member(*attnums, var->varattno);

		return true;
	}

	/* (Var/Expr op Const) or (Const op Var/Expr) */
	if (is_opclause(clause))
	{
		RangeTblEntry *rte = root->simple_rte_array[relid];
		OpExpr	   *expr = (OpExpr *) clause;
		Node	   *clause_expr;

		/* Only expressions with two arguments are considered compatible. */
		if (list_length(expr->args) != 2)
			return false;

		/* Check if the expression has the right shape */
		if (!examine_opclause_args(expr->args, &clause_expr, NULL, NULL))
			return false;

		/*
		 * If it's not one of the supported operators ("=", "<", ">", etc.),
		 * just ignore the clause, as it's not compatible with MCV lists.
		 *
		 * This uses the function for estimating selectivity, not the operator
		 * directly (a bit awkward, but well ...).
		 */
		switch (get_oprrest(expr->opno))
		{
			case F_EQSEL:
			case F_NEQSEL:
			case F_SCALARLTSEL:
			case F_SCALARLESEL:
			case F_SCALARGTSEL:
			case F_SCALARGESEL:
				/* supported, will continue with inspection of the Var/Expr */
				break;

			default:
				/* other estimators are considered unknown/unsupported */
				return false;
		}

		/*
		 * If there are any securityQuals on the RTE from security barrier
		 * views or RLS policies, then the user may not have access to all the
		 * table's data, and we must check that the operator is leakproof.
		 *
		 * If the operator is leaky, then we must ignore this clause for the
		 * purposes of estimating with MCV lists, otherwise the operator might
		 * reveal values from the MCV list that the user doesn't have
		 * permission to see.
		 */
		if (rte->securityQuals != NIL &&
			!get_func_leakproof(get_opcode(expr->opno)))
			return false;

		/* Check (Var op Const) or (Const op Var) clauses by recursing. */
		if (IsA(clause_expr, Var))
			return statext_is_compatible_clause_internal(root, clause_expr,
														 relid, attnums, exprs);

		/* Otherwise we have (Expr op Const) or (Const op Expr). */
		*exprs = lappend(*exprs, clause_expr);
		return true;
	}

	/* Var/Expr IN Array */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		RangeTblEntry *rte = root->simple_rte_array[relid];
		ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) clause;
		Node	   *clause_expr;
		bool		expronleft;

		/* Only expressions with two arguments are considered compatible. */
		if (list_length(expr->args) != 2)
			return false;

		/* Check if the expression has the right shape (one Var, one Const) */
		if (!examine_opclause_args(expr->args, &clause_expr, NULL, &expronleft))
			return false;

		/* We only support Var on left, Const on right */
		if (!expronleft)
			return false;

		/*
		 * If it's not one of the supported operators ("=", "<", ">", etc.),
		 * just ignore the clause, as it's not compatible with MCV lists.
		 *
		 * This uses the function for estimating selectivity, not the operator
		 * directly (a bit awkward, but well ...).
		 */
		switch (get_oprrest(expr->opno))
		{
			case F_EQSEL:
			case F_NEQSEL:
			case F_SCALARLTSEL:
			case F_SCALARLESEL:
			case F_SCALARGTSEL:
			case F_SCALARGESEL:
				/* supported, will continue with inspection of the Var/Expr */
				break;

			default:
				/* other estimators are considered unknown/unsupported */
				return false;
		}

		/*
		 * If there are any securityQuals on the RTE from security barrier
		 * views or RLS policies, then the user may not have access to all the
		 * table's data, and we must check that the operator is leakproof.
		 *
		 * If the operator is leaky, then we must ignore this clause for the
		 * purposes of estimating with MCV lists, otherwise the operator might
		 * reveal values from the MCV list that the user doesn't have
		 * permission to see.
		 */
		if (rte->securityQuals != NIL &&
			!get_func_leakproof(get_opcode(expr->opno)))
			return false;

		/* Check Var IN Array clauses by recursing. */
		if (IsA(clause_expr, Var))
			return statext_is_compatible_clause_internal(root, clause_expr,
														 relid, attnums, exprs);

		/* Otherwise we have Expr IN Array. */
		*exprs = lappend(*exprs, clause_expr);
		return true;
	}

	/* AND/OR/NOT clause */
	if (is_andclause(clause) ||
		is_orclause(clause) ||
		is_notclause(clause))
	{
		/*
		 * AND/OR/NOT-clauses are supported if all sub-clauses are supported
		 *
		 * Perhaps we could improve this by handling mixed cases, when some of
		 * the clauses are supported and some are not. Selectivity for the
		 * supported subclauses would be computed using extended statistics,
		 * and the remaining clauses would be estimated using the traditional
		 * algorithm (product of selectivities).
		 *
		 * It however seems overly complex, and in a way we already do that
		 * because if we reject the whole clause as unsupported here, it will
		 * be eventually passed to clauselist_selectivity() which does exactly
		 * this (split into supported/unsupported clauses etc).
		 */
		BoolExpr   *expr = (BoolExpr *) clause;
		ListCell   *lc;

		foreach(lc, expr->args)
		{
			/*
			 * If we find an incompatible clause in the arguments, treat the
			 * whole clause as incompatible.
			 */
			if (!statext_is_compatible_clause_internal(root,
													   (Node *) lfirst(lc),
													   relid, attnums, exprs))
				return false;
		}

		return true;
	}

	/* Var/Expr IS NULL */
	if (IsA(clause, NullTest))
	{
		NullTest   *nt = (NullTest *) clause;

		/* Check Var IS NULL clauses by recursing. */
		if (IsA(nt->arg, Var))
			return statext_is_compatible_clause_internal(root, (Node *) (nt->arg),
														 relid, attnums, exprs);

		/* Otherwise we have Expr IS NULL. */
		*exprs = lappend(*exprs, nt->arg);
		return true;
	}

	/*
	 * Treat any other expressions as bare expressions to be matched against
	 * expressions in statistics objects.
	 */
	*exprs = lappend(*exprs, clause);
	return true;
}

/*
 * statext_is_compatible_clause
 *		Determines if the clause is compatible with MCV lists.
 *
 * See statext_is_compatible_clause_internal, above, for the basic rules.
 * This layer deals with RestrictInfo superstructure and applies permissions
 * checks to verify that it's okay to examine all mentioned Vars.
 *
 * Arguments:
 * clause: clause to be inspected (in RestrictInfo form)
 * relid: rel that all Vars in clause must belong to
 * *attnums: input/output parameter collecting attribute numbers of all
 *		mentioned Vars.  Note that we do not offset the attribute numbers,
 *		so we can't cope with system columns.
 * *exprs: input/output parameter collecting primitive subclauses within
 *		the clause tree
 *
 * Returns false if there is something we definitively can't handle.
 * On true return, we can proceed to match the *exprs against statistics.
 */
static bool
statext_is_compatible_clause(PlannerInfo *root, Node *clause, Index relid,
							 Bitmapset **attnums, List **exprs)
{
	RangeTblEntry *rte = root->simple_rte_array[relid];
	RelOptInfo *rel = root->simple_rel_array[relid];
	RestrictInfo *rinfo;
	int			clause_relid;
	Oid			userid;

	/*
	 * Special-case handling for bare BoolExpr AND clauses, because the
	 * restrictinfo machinery doesn't build RestrictInfos on top of AND
	 * clauses.
	 */
	if (is_andclause(clause))
	{
		BoolExpr   *expr = (BoolExpr *) clause;
		ListCell   *lc;

		/*
		 * Check that each sub-clause is compatible.  We expect these to be
		 * RestrictInfos.
		 */
		foreach(lc, expr->args)
		{
			if (!statext_is_compatible_clause(root, (Node *) lfirst(lc),
											  relid, attnums, exprs))
				return false;
		}

		return true;
	}

	/* Otherwise it must be a RestrictInfo. */
	if (!IsA(clause, RestrictInfo))
		return false;
	rinfo = (RestrictInfo *) clause;

	/* Pseudoconstants are not really interesting here. */
	if (rinfo->pseudoconstant)
		return false;

	/* Clauses referencing other varnos are incompatible. */
	if (!bms_get_singleton_member(rinfo->clause_relids, &clause_relid) ||
		clause_relid != relid)
		return false;

	/* Check the clause and determine what attributes it references. */
	if (!statext_is_compatible_clause_internal(root, (Node *) rinfo->clause,
											   relid, attnums, exprs))
		return false;

	/*
	 * Check that the user has permission to read all required attributes.
	 */
	userid = OidIsValid(rel->userid) ? rel->userid : GetUserId();

	/* Table-level SELECT privilege is sufficient for all columns */
	if (pg_class_aclcheck(rte->relid, userid, ACL_SELECT) != ACLCHECK_OK)
	{
		Bitmapset  *clause_attnums = NULL;
		int			attnum = -1;

		/*
		 * We have to check per-column privileges.  *attnums has the attnums
		 * for individual Vars we saw, but there may also be Vars within
		 * subexpressions in *exprs.  We can use pull_varattnos() to extract
		 * those, but there's an impedance mismatch: attnums returned by
		 * pull_varattnos() are offset by FirstLowInvalidHeapAttributeNumber,
		 * while attnums within *attnums aren't.  Convert *attnums to the
		 * offset style so we can combine the results.
		 */
		while ((attnum = bms_next_member(*attnums, attnum)) >= 0)
		{
			clause_attnums =
				bms_add_member(clause_attnums,
							   attnum - FirstLowInvalidHeapAttributeNumber);
		}

		/* Now merge attnums from *exprs into clause_attnums */
		if (*exprs != NIL)
			pull_varattnos((Node *) *exprs, relid, &clause_attnums);

		attnum = -1;
		while ((attnum = bms_next_member(clause_attnums, attnum)) >= 0)
		{
			/* Undo the offset */
			AttrNumber	attno = attnum + FirstLowInvalidHeapAttributeNumber;

			if (attno == InvalidAttrNumber)
			{
				/* Whole-row reference, so must have access to all columns */
				if (pg_attribute_aclcheck_all(rte->relid, userid, ACL_SELECT,
											  ACLMASK_ALL) != ACLCHECK_OK)
					return false;
			}
			else
			{
				if (pg_attribute_aclcheck(rte->relid, attno, userid,
										  ACL_SELECT) != ACLCHECK_OK)
					return false;
			}
		}
	}

	/* If we reach here, the clause is OK */
	return true;
}

/*
 * statext_mcv_clauselist_selectivity
 *		Estimate clauses using the best multi-column statistics.
 *
 * Applies available extended (multi-column) statistics on a table. There may
 * be multiple applicable statistics (with respect to the clauses), in which
 * case we use greedy approach. In each round we select the best statistic on
 * a table (measured by the number of attributes extracted from the clauses
 * and covered by it), and compute the selectivity for the supplied clauses.
 * We repeat this process with the remaining clauses (if any), until none of
 * the available statistics can be used.
 *
 * One of the main challenges with using MCV lists is how to extrapolate the
 * estimate to the data not covered by the MCV list. To do that, we compute
 * not only the "MCV selectivity" (selectivities for MCV items matching the
 * supplied clauses), but also the following related selectivities:
 *
 * - simple selectivity:  Computed without extended statistics, i.e. as if the
 * columns/clauses were independent.
 *
 * - base selectivity:  Similar to simple selectivity, but is computed using
 * the extended statistic by adding up the base frequencies (that we compute
 * and store for each MCV item) of matching MCV items.
 *
 * - total selectivity: Selectivity covered by the whole MCV list.
 *
 * These are passed to mcv_combine_selectivities() which combines them to
 * produce a selectivity estimate that makes use of both per-column statistics
 * and the multi-column MCV statistics.
 *
 * 'estimatedclauses' is an input/output parameter.  We set bits for the
 * 0-based 'clauses' indexes we estimate for and also skip clause items that
 * already have a bit set.
 */
static Selectivity
statext_mcv_clauselist_selectivity(PlannerInfo *root, List *clauses, int varRelid,
								   JoinType jointype, SpecialJoinInfo *sjinfo,
								   RelOptInfo *rel, Bitmapset **estimatedclauses,
								   bool is_or)
{
	ListCell   *l;
	Bitmapset **list_attnums;	/* attnums extracted from the clause */
	List	  **list_exprs;		/* expressions matched to any statistic */
	int			listidx;
	Selectivity sel = (is_or) ? 0.0 : 1.0;
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	/* check if there's any stats that might be useful for us. */
	if (!has_stats_of_kind(rel->statlist, STATS_EXT_MCV))
		return sel;

	list_attnums = (Bitmapset **) palloc(sizeof(Bitmapset *) *
										 list_length(clauses));

	/* expressions extracted from complex expressions */
	list_exprs = (List **) palloc(sizeof(Node *) * list_length(clauses));

	/*
	 * Pre-process the clauses list to extract the attnums and expressions
	 * seen in each item.  We need to determine if there are any clauses which
	 * will be useful for selectivity estimations with extended stats.  Along
	 * the way we'll record all of the attnums and expressions for each clause
	 * in lists which we'll reference later so we don't need to repeat the
	 * same work again.
	 *
	 * We also skip clauses that we already estimated using different types of
	 * statistics (we treat them as incompatible).
	 */
	listidx = 0;
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);
		Bitmapset  *attnums = NULL;
		List	   *exprs = NIL;

		if (!bms_is_member(listidx, *estimatedclauses) &&
			statext_is_compatible_clause(root, clause, rel->relid, &attnums, &exprs))
		{
			list_attnums[listidx] = attnums;
			list_exprs[listidx] = exprs;
		}
		else
		{
			list_attnums[listidx] = NULL;
			list_exprs[listidx] = NIL;
		}

		listidx++;
	}

	/* apply as many extended statistics as possible */
	while (true)
	{
		StatisticExtInfo *stat;
		List	   *stat_clauses;
		Bitmapset  *simple_clauses;

		/* find the best suited statistics object for these attnums */
		stat = choose_best_statistics(rel->statlist, STATS_EXT_MCV, rte->inh,
									  list_attnums, list_exprs,
									  list_length(clauses));

		/*
		 * if no (additional) matching stats could be found then we've nothing
		 * to do
		 */
		if (!stat)
			break;

		/* Ensure choose_best_statistics produced an expected stats type. */
		Assert(stat->kind == STATS_EXT_MCV);

		/* now filter the clauses to be estimated using the selected MCV */
		stat_clauses = NIL;

		/* record which clauses are simple (single column or expression) */
		simple_clauses = NULL;

		listidx = -1;
		foreach(l, clauses)
		{
			/* Increment the index before we decide if to skip the clause. */
			listidx++;

			/*
			 * Ignore clauses from which we did not extract any attnums or
			 * expressions (this needs to be consistent with what we do in
			 * choose_best_statistics).
			 *
			 * This also eliminates already estimated clauses - both those
			 * estimated before and during applying extended statistics.
			 *
			 * XXX This check is needed because both bms_is_subset and
			 * stat_covers_expressions return true for empty attnums and
			 * expressions.
			 */
			if (!list_attnums[listidx] && !list_exprs[listidx])
				continue;

			/*
			 * The clause was not estimated yet, and we've extracted either
			 * attnums or expressions from it. Ignore it if it's not fully
			 * covered by the chosen statistics object.
			 *
			 * We need to check both attributes and expressions, and reject if
			 * either is not covered.
			 */
			if (!bms_is_subset(list_attnums[listidx], stat->keys) ||
				!stat_covers_expressions(stat, list_exprs[listidx], NULL))
				continue;

			/*
			 * Now we know the clause is compatible (we have either attnums or
			 * expressions extracted from it), and was not estimated yet.
			 */

			/* record simple clauses (single column or expression) */
			if ((list_attnums[listidx] == NULL &&
				 list_length(list_exprs[listidx]) == 1) ||
				(list_exprs[listidx] == NIL &&
				 bms_membership(list_attnums[listidx]) == BMS_SINGLETON))
				simple_clauses = bms_add_member(simple_clauses,
												list_length(stat_clauses));

			/* add clause to list and mark it as estimated */
			stat_clauses = lappend(stat_clauses, (Node *) lfirst(l));
			*estimatedclauses = bms_add_member(*estimatedclauses, listidx);

			/*
			 * Reset the pointers, so that choose_best_statistics knows this
			 * clause was estimated and does not consider it again.
			 */
			bms_free(list_attnums[listidx]);
			list_attnums[listidx] = NULL;

			list_free(list_exprs[listidx]);
			list_exprs[listidx] = NULL;
		}

		if (is_or)
		{
			bool	   *or_matches = NULL;
			Selectivity simple_or_sel = 0.0,
						stat_sel = 0.0;
			MCVList    *mcv_list;

			/* Load the MCV list stored in the statistics object */
			mcv_list = statext_mcv_load(stat->statOid, rte->inh);

			/*
			 * Compute the selectivity of the ORed list of clauses covered by
			 * this statistics object by estimating each in turn and combining
			 * them using the formula P(A OR B) = P(A) + P(B) - P(A AND B).
			 * This allows us to use the multivariate MCV stats to better
			 * estimate the individual terms and their overlap.
			 *
			 * Each time we iterate this formula, the clause "A" above is
			 * equal to all the clauses processed so far, combined with "OR".
			 */
			listidx = 0;
			foreach(l, stat_clauses)
			{
				Node	   *clause = (Node *) lfirst(l);
				Selectivity simple_sel,
							overlap_simple_sel,
							mcv_sel,
							mcv_basesel,
							overlap_mcvsel,
							overlap_basesel,
							mcv_totalsel,
							clause_sel,
							overlap_sel;

				/*
				 * "Simple" selectivity of the next clause and its overlap
				 * with any of the previous clauses.  These are our initial
				 * estimates of P(B) and P(A AND B), assuming independence of
				 * columns/clauses.
				 */
				simple_sel = clause_selectivity_ext(root, clause, varRelid,
													jointype, sjinfo, false);

				overlap_simple_sel = simple_or_sel * simple_sel;

				/*
				 * New "simple" selectivity of all clauses seen so far,
				 * assuming independence.
				 */
				simple_or_sel += simple_sel - overlap_simple_sel;
				CLAMP_PROBABILITY(simple_or_sel);

				/*
				 * Multi-column estimate of this clause using MCV statistics,
				 * along with base and total selectivities, and corresponding
				 * selectivities for the overlap term P(A AND B).
				 */
				mcv_sel = mcv_clause_selectivity_or(root, stat, mcv_list,
													clause, &or_matches,
													&mcv_basesel,
													&overlap_mcvsel,
													&overlap_basesel,
													&mcv_totalsel);

				/*
				 * Combine the simple and multi-column estimates.
				 *
				 * If this clause is a simple single-column clause, then we
				 * just use the simple selectivity estimate for it, since the
				 * multi-column statistics are unlikely to improve on that
				 * (and in fact could make it worse).  For the overlap, we
				 * always make use of the multi-column statistics.
				 */
				if (bms_is_member(listidx, simple_clauses))
					clause_sel = simple_sel;
				else
					clause_sel = mcv_combine_selectivities(simple_sel,
														   mcv_sel,
														   mcv_basesel,
														   mcv_totalsel);

				overlap_sel = mcv_combine_selectivities(overlap_simple_sel,
														overlap_mcvsel,
														overlap_basesel,
														mcv_totalsel);

				/* Factor these into the result for this statistics object */
				stat_sel += clause_sel - overlap_sel;
				CLAMP_PROBABILITY(stat_sel);

				listidx++;
			}

			/*
			 * Factor the result for this statistics object into the overall
			 * result.  We treat the results from each separate statistics
			 * object as independent of one another.
			 */
			sel = sel + stat_sel - sel * stat_sel;
		}
		else					/* Implicitly-ANDed list of clauses */
		{
			Selectivity simple_sel,
						mcv_sel,
						mcv_basesel,
						mcv_totalsel,
						stat_sel;

			/*
			 * "Simple" selectivity, i.e. without any extended statistics,
			 * essentially assuming independence of the columns/clauses.
			 */
			simple_sel = clauselist_selectivity_ext(root, stat_clauses,
													varRelid, jointype,
													sjinfo, false);

			/*
			 * Multi-column estimate using MCV statistics, along with base and
			 * total selectivities.
			 */
			mcv_sel = mcv_clauselist_selectivity(root, stat, stat_clauses,
												 varRelid, jointype, sjinfo,
												 rel, &mcv_basesel,
												 &mcv_totalsel);

			/* Combine the simple and multi-column estimates. */
			stat_sel = mcv_combine_selectivities(simple_sel,
												 mcv_sel,
												 mcv_basesel,
												 mcv_totalsel);

			/* Factor this into the overall result */
			sel *= stat_sel;
		}
	}

	return sel;
}

/*
 * statext_clauselist_selectivity
 *		Estimate clauses using the best multi-column statistics.
 */
Selectivity
statext_clauselist_selectivity(PlannerInfo *root, List *clauses, int varRelid,
							   JoinType jointype, SpecialJoinInfo *sjinfo,
							   RelOptInfo *rel, Bitmapset **estimatedclauses,
							   bool is_or)
{
	Selectivity sel;

	/* First, try estimating clauses using a multivariate MCV list. */
	sel = statext_mcv_clauselist_selectivity(root, clauses, varRelid, jointype,
											 sjinfo, rel, estimatedclauses, is_or);

	/*
	 * Functional dependencies only work for clauses connected by AND, so for
	 * OR clauses we're done.
	 */
	if (is_or)
		return sel;

	/*
	 * Then, apply functional dependencies on the remaining clauses by calling
	 * dependencies_clauselist_selectivity.  Pass 'estimatedclauses' so the
	 * function can properly skip clauses already estimated above.
	 *
	 * The reasoning for applying dependencies last is that the more complex
	 * stats can track more complex correlations between the attributes, and
	 * so may be considered more reliable.
	 *
	 * For example, MCV list can give us an exact selectivity for values in
	 * two columns, while functional dependencies can only provide information
	 * about the overall strength of the dependency.
	 */
	sel *= dependencies_clauselist_selectivity(root, clauses, varRelid,
											   jointype, sjinfo, rel,
											   estimatedclauses);

	return sel;
}

/*
 * examine_opclause_args
 *		Split an operator expression's arguments into Expr and Const parts.
 *
 * Attempts to match the arguments to either (Expr op Const) or (Const op
 * Expr), possibly with a RelabelType on top. When the expression matches this
 * form, returns true, otherwise returns false.
 *
 * Optionally returns pointers to the extracted Expr/Const nodes, when passed
 * non-null pointers (exprp, cstp and expronleftp). The expronleftp flag
 * specifies on which side of the operator we found the expression node.
 */
bool
examine_opclause_args(List *args, Node **exprp, Const **cstp,
					  bool *expronleftp)
{
	Node	   *expr;
	Const	   *cst;
	bool		expronleft;
	Node	   *leftop,
			   *rightop;

	/* enforced by statext_is_compatible_clause_internal */
	Assert(list_length(args) == 2);

	leftop = linitial(args);
	rightop = lsecond(args);

	/* strip RelabelType from either side of the expression */
	if (IsA(leftop, RelabelType))
		leftop = (Node *) ((RelabelType *) leftop)->arg;

	if (IsA(rightop, RelabelType))
		rightop = (Node *) ((RelabelType *) rightop)->arg;

	if (IsA(rightop, Const))
	{
		expr = (Node *) leftop;
		cst = (Const *) rightop;
		expronleft = true;
	}
	else if (IsA(leftop, Const))
	{
		expr = (Node *) rightop;
		cst = (Const *) leftop;
		expronleft = false;
	}
	else
		return false;

	/* return pointers to the extracted parts if requested */
	if (exprp)
		*exprp = expr;

	if (cstp)
		*cstp = cst;

	if (expronleftp)
		*expronleftp = expronleft;

	return true;
}


/*
 * Compute statistics about expressions of a relation.
 */
static void
compute_expr_stats(Relation onerel, AnlExprData *exprdata, int nexprs,
				   HeapTuple *rows, int numrows)
{
	MemoryContext expr_context,
				old_context;
	int			ind,
				i;

	expr_context = AllocSetContextCreate(CurrentMemoryContext,
										 "Analyze Expression",
										 ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(expr_context);

	for (ind = 0; ind < nexprs; ind++)
	{
		AnlExprData *thisdata = &exprdata[ind];
		VacAttrStats *stats = thisdata->vacattrstat;
		Node	   *expr = thisdata->expr;
		TupleTableSlot *slot;
		EState	   *estate;
		ExprContext *econtext;
		Datum	   *exprvals;
		bool	   *exprnulls;
		ExprState  *exprstate;
		int			tcnt;

		/* Are we still in the main context? */
		Assert(CurrentMemoryContext == expr_context);

		/*
		 * Need an EState for evaluation of expressions.  Create it in the
		 * per-expression context to be sure it gets cleaned up at the bottom
		 * of the loop.
		 */
		estate = CreateExecutorState();
		econtext = GetPerTupleExprContext(estate);

		/* Set up expression evaluation state */
		exprstate = ExecPrepareExpr((Expr *) expr, estate);

		/* Need a slot to hold the current heap tuple, too */
		slot = MakeSingleTupleTableSlot(RelationGetDescr(onerel),
										&TTSOpsHeapTuple);

		/* Arrange for econtext's scan tuple to be the tuple under test */
		econtext->ecxt_scantuple = slot;

		/* Compute and save expression values */
		exprvals = (Datum *) palloc(numrows * sizeof(Datum));
		exprnulls = (bool *) palloc(numrows * sizeof(bool));

		tcnt = 0;
		for (i = 0; i < numrows; i++)
		{
			Datum		datum;
			bool		isnull;

			/*
			 * Reset the per-tuple context each time, to reclaim any cruft
			 * left behind by evaluating the statistics expressions.
			 */
			ResetExprContext(econtext);

			/* Set up for expression evaluation */
			ExecStoreHeapTuple(rows[i], slot, false);

			/*
			 * Evaluate the expression. We do this in the per-tuple context so
			 * as not to leak memory, and then copy the result into the
			 * context created at the beginning of this function.
			 */
			datum = ExecEvalExprSwitchContext(exprstate,
											  GetPerTupleExprContext(estate),
											  &isnull);
			if (isnull)
			{
				exprvals[tcnt] = (Datum) 0;
				exprnulls[tcnt] = true;
			}
			else
			{
				/* Make sure we copy the data into the context. */
				Assert(CurrentMemoryContext == expr_context);

				exprvals[tcnt] = datumCopy(datum,
										   stats->attrtype->typbyval,
										   stats->attrtype->typlen);
				exprnulls[tcnt] = false;
			}

			tcnt++;
		}

		/*
		 * Now we can compute the statistics for the expression columns.
		 *
		 * XXX Unlike compute_index_stats we don't need to switch and reset
		 * memory contexts here, because we're only computing stats for a
		 * single expression (and not iterating over many indexes), so we just
		 * do it in expr_context. Note that compute_stats copies the result
		 * into stats->anl_context, so it does not disappear.
		 */
		if (tcnt > 0)
		{
			AttributeOpts *aopt =
				get_attribute_options(onerel->rd_id, stats->tupattnum);

			stats->exprvals = exprvals;
			stats->exprnulls = exprnulls;
			stats->rowstride = 1;
			stats->compute_stats(stats,
								 expr_fetch_func,
								 tcnt,
								 tcnt);

			/*
			 * If the n_distinct option is specified, it overrides the above
			 * computation.
			 */
			if (aopt != NULL && aopt->n_distinct != 0.0)
				stats->stadistinct = aopt->n_distinct;
		}

		/* And clean up */
		MemoryContextSwitchTo(expr_context);

		ExecDropSingleTupleTableSlot(slot);
		FreeExecutorState(estate);
		MemoryContextReset(expr_context);
	}

	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(expr_context);
}


/*
 * Fetch function for analyzing statistics object expressions.
 *
 * We have not bothered to construct tuples from the data, instead the data
 * is just in Datum arrays.
 */
static Datum
expr_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			i;

	/* exprvals and exprnulls are already offset for proper column */
	i = rownum * stats->rowstride;
	*isNull = stats->exprnulls[i];
	return stats->exprvals[i];
}

/*
 * Build analyze data for a list of expressions. As this is not tied
 * directly to a relation (table or index), we have to fake some of
 * the fields in examine_expression().
 */
static AnlExprData *
build_expr_data(List *exprs, int stattarget)
{
	int			idx;
	int			nexprs = list_length(exprs);
	AnlExprData *exprdata;
	ListCell   *lc;

	exprdata = (AnlExprData *) palloc0(nexprs * sizeof(AnlExprData));

	idx = 0;
	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		AnlExprData *thisdata = &exprdata[idx];

		thisdata->expr = expr;
		thisdata->vacattrstat = examine_expression(expr, stattarget);
		idx++;
	}

	return exprdata;
}

/* form an array of pg_statistic rows (per update_attstats) */
static Datum
serialize_expr_stats(AnlExprData *exprdata, int nexprs)
{
	int			exprno;
	Oid			typOid;
	Relation	sd;

	ArrayBuildState *astate = NULL;

	sd = table_open(StatisticRelationId, RowExclusiveLock);

	/* lookup OID of composite type for pg_statistic */
	typOid = get_rel_type_id(StatisticRelationId);
	if (!OidIsValid(typOid))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" does not have a composite type",
						"pg_statistic")));

	for (exprno = 0; exprno < nexprs; exprno++)
	{
		int			i,
					k;
		VacAttrStats *stats = exprdata[exprno].vacattrstat;

		Datum		values[Natts_pg_statistic];
		bool		nulls[Natts_pg_statistic];
		HeapTuple	stup;

		if (!stats->stats_valid)
		{
			astate = accumArrayResult(astate,
									  (Datum) 0,
									  true,
									  typOid,
									  CurrentMemoryContext);
			continue;
		}

		/*
		 * Construct a new pg_statistic tuple
		 */
		for (i = 0; i < Natts_pg_statistic; ++i)
		{
			nulls[i] = false;
		}

		values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(InvalidOid);
		values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(InvalidAttrNumber);
		values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(false);
		values[Anum_pg_statistic_stanullfrac - 1] = Float4GetDatum(stats->stanullfrac);
		values[Anum_pg_statistic_stawidth - 1] = Int32GetDatum(stats->stawidth);
		values[Anum_pg_statistic_stadistinct - 1] = Float4GetDatum(stats->stadistinct);
		i = Anum_pg_statistic_stakind1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = Int16GetDatum(stats->stakind[k]); /* stakindN */
		}
		i = Anum_pg_statistic_staop1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->staop[k]);	/* staopN */
		}
		i = Anum_pg_statistic_stacoll1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->stacoll[k]);	/* stacollN */
		}
		i = Anum_pg_statistic_stanumbers1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			int			nnum = stats->numnumbers[k];

			if (nnum > 0)
			{
				int			n;
				Datum	   *numdatums = (Datum *) palloc(nnum * sizeof(Datum));
				ArrayType  *arry;

				for (n = 0; n < nnum; n++)
					numdatums[n] = Float4GetDatum(stats->stanumbers[k][n]);
				arry = construct_array_builtin(numdatums, nnum, FLOAT4OID);
				values[i++] = PointerGetDatum(arry);	/* stanumbersN */
			}
			else
			{
				nulls[i] = true;
				values[i++] = (Datum) 0;
			}
		}
		i = Anum_pg_statistic_stavalues1 - 1;
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			if (stats->numvalues[k] > 0)
			{
				ArrayType  *arry;

				arry = construct_array(stats->stavalues[k],
									   stats->numvalues[k],
									   stats->statypid[k],
									   stats->statyplen[k],
									   stats->statypbyval[k],
									   stats->statypalign[k]);
				values[i++] = PointerGetDatum(arry);	/* stavaluesN */
			}
			else
			{
				nulls[i] = true;
				values[i++] = (Datum) 0;
			}
		}

		stup = heap_form_tuple(RelationGetDescr(sd), values, nulls);

		astate = accumArrayResult(astate,
								  heap_copy_tuple_as_datum(stup, RelationGetDescr(sd)),
								  false,
								  typOid,
								  CurrentMemoryContext);
	}

	table_close(sd, RowExclusiveLock);

	return makeArrayResult(astate, CurrentMemoryContext);
}

/*
 * Loads pg_statistic record from expression statistics for expression
 * identified by the supplied index.
 */
HeapTuple
statext_expressions_load(Oid stxoid, bool inh, int idx)
{
	bool		isnull;
	Datum		value;
	HeapTuple	htup;
	ExpandedArrayHeader *eah;
	HeapTupleHeader td;
	HeapTupleData tmptup;
	HeapTuple	tup;

	htup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(stxoid), BoolGetDatum(inh));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", stxoid);

	value = SysCacheGetAttr(STATEXTDATASTXOID, htup,
							Anum_pg_statistic_ext_data_stxdexpr, &isnull);
	if (isnull)
		elog(ERROR,
			 "requested statistics kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_EXPRESSIONS, stxoid);

	eah = DatumGetExpandedArray(value);

	deconstruct_expanded_array(eah);

	td = DatumGetHeapTupleHeader(eah->dvalues[idx]);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = td;

	tup = heap_copytuple(&tmptup);

	ReleaseSysCache(htup);

	return tup;
}

/*
 * Evaluate the expressions, so that we can use the results to build
 * all the requested statistics types. This matters especially for
 * expensive expressions, of course.
 */
static StatsBuildData *
make_build_data(Relation rel, StatExtEntry *stat, int numrows, HeapTuple *rows,
				VacAttrStats **stats, int stattarget)
{
	/* evaluated expressions */
	StatsBuildData *result;
	char	   *ptr;
	Size		len;

	int			i;
	int			k;
	int			idx;
	TupleTableSlot *slot;
	EState	   *estate;
	ExprContext *econtext;
	List	   *exprstates = NIL;
	int			nkeys = bms_num_members(stat->columns) + list_length(stat->exprs);
	ListCell   *lc;

	/* allocate everything as a single chunk, so we can free it easily */
	len = MAXALIGN(sizeof(StatsBuildData));
	len += MAXALIGN(sizeof(AttrNumber) * nkeys);	/* attnums */
	len += MAXALIGN(sizeof(VacAttrStats *) * nkeys);	/* stats */

	/* values */
	len += MAXALIGN(sizeof(Datum *) * nkeys);
	len += nkeys * MAXALIGN(sizeof(Datum) * numrows);

	/* nulls */
	len += MAXALIGN(sizeof(bool *) * nkeys);
	len += nkeys * MAXALIGN(sizeof(bool) * numrows);

	ptr = palloc(len);

	/* set the pointers */
	result = (StatsBuildData *) ptr;
	ptr += MAXALIGN(sizeof(StatsBuildData));

	/* attnums */
	result->attnums = (AttrNumber *) ptr;
	ptr += MAXALIGN(sizeof(AttrNumber) * nkeys);

	/* stats */
	result->stats = (VacAttrStats **) ptr;
	ptr += MAXALIGN(sizeof(VacAttrStats *) * nkeys);

	/* values */
	result->values = (Datum **) ptr;
	ptr += MAXALIGN(sizeof(Datum *) * nkeys);

	/* nulls */
	result->nulls = (bool **) ptr;
	ptr += MAXALIGN(sizeof(bool *) * nkeys);

	for (i = 0; i < nkeys; i++)
	{
		result->values[i] = (Datum *) ptr;
		ptr += MAXALIGN(sizeof(Datum) * numrows);

		result->nulls[i] = (bool *) ptr;
		ptr += MAXALIGN(sizeof(bool) * numrows);
	}

	Assert((ptr - (char *) result) == len);

	/* we have it allocated, so let's fill the values */
	result->nattnums = nkeys;
	result->numrows = numrows;

	/* fill the attribute info - first attributes, then expressions */
	idx = 0;
	k = -1;
	while ((k = bms_next_member(stat->columns, k)) >= 0)
	{
		result->attnums[idx] = k;
		result->stats[idx] = stats[idx];

		idx++;
	}

	k = -1;
	foreach(lc, stat->exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);

		result->attnums[idx] = k;
		result->stats[idx] = examine_expression(expr, stattarget);

		idx++;
		k--;
	}

	/* first extract values for all the regular attributes */
	for (i = 0; i < numrows; i++)
	{
		idx = 0;
		k = -1;
		while ((k = bms_next_member(stat->columns, k)) >= 0)
		{
			result->values[idx][i] = heap_getattr(rows[i], k,
												  result->stats[idx]->tupDesc,
												  &result->nulls[idx][i]);

			idx++;
		}
	}

	/* Need an EState for evaluation expressions. */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);

	/* Need a slot to hold the current heap tuple, too */
	slot = MakeSingleTupleTableSlot(RelationGetDescr(rel),
									&TTSOpsHeapTuple);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Set up expression evaluation state */
	exprstates = ExecPrepareExprList(stat->exprs, estate);

	for (i = 0; i < numrows; i++)
	{
		/*
		 * Reset the per-tuple context each time, to reclaim any cruft left
		 * behind by evaluating the statistics object expressions.
		 */
		ResetExprContext(econtext);

		/* Set up for expression evaluation */
		ExecStoreHeapTuple(rows[i], slot, false);

		idx = bms_num_members(stat->columns);
		foreach(lc, exprstates)
		{
			Datum		datum;
			bool		isnull;
			ExprState  *exprstate = (ExprState *) lfirst(lc);

			/*
			 * XXX This probably leaks memory. Maybe we should use
			 * ExecEvalExprSwitchContext but then we need to copy the result
			 * somewhere else.
			 */
			datum = ExecEvalExpr(exprstate,
								 GetPerTupleExprContext(estate),
								 &isnull);
			if (isnull)
			{
				result->values[idx][i] = (Datum) 0;
				result->nulls[idx][i] = true;
			}
			else
			{
				result->values[idx][i] = (Datum) datum;
				result->nulls[idx][i] = false;
			}

			idx++;
		}
	}

	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);

	return result;
}
