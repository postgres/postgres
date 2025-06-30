/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  the Postgres statistics generator
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/analyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/tupconvert.h"
#include "access/visibilitymap.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits.h"
#include "commands/dbcommands.h"
#include "commands/progress.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/attoptcache.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/sampling.h"
#include "utils/sortsupport.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/* Per-index data for ANALYZE */
typedef struct AnlIndexData
{
	IndexInfo  *indexInfo;		/* BuildIndexInfo result */
	double		tupleFract;		/* fraction of rows for partial index */
	VacAttrStats **vacattrstats;	/* index attrs to analyze */
	int			attr_cnt;
} AnlIndexData;


/* Default statistics target (GUC parameter) */
int			default_statistics_target = 100;

/* A few variables that don't seem worth passing around as parameters */
static MemoryContext anl_context = NULL;
static BufferAccessStrategy vac_strategy;


static void do_analyze_rel(Relation onerel,
						   const VacuumParams params, List *va_cols,
						   AcquireSampleRowsFunc acquirefunc, BlockNumber relpages,
						   bool inh, bool in_outer_xact, int elevel);
static void compute_index_stats(Relation onerel, double totalrows,
								AnlIndexData *indexdata, int nindexes,
								HeapTuple *rows, int numrows,
								MemoryContext col_context);
static VacAttrStats *examine_attribute(Relation onerel, int attnum,
									   Node *index_expr);
static int	acquire_sample_rows(Relation onerel, int elevel,
								HeapTuple *rows, int targrows,
								double *totalrows, double *totaldeadrows);
static int	compare_rows(const void *a, const void *b, void *arg);
static int	acquire_inherited_sample_rows(Relation onerel, int elevel,
										  HeapTuple *rows, int targrows,
										  double *totalrows, double *totaldeadrows);
static void update_attstats(Oid relid, bool inh,
							int natts, VacAttrStats **vacattrstats);
static Datum std_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull);
static Datum ind_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull);


/*
 *	analyze_rel() -- analyze one relation
 *
 * relid identifies the relation to analyze.  If relation is supplied, use
 * the name therein for reporting any failure to open/lock the rel; do not
 * use it once we've successfully opened the rel, since it might be stale.
 */
void
analyze_rel(Oid relid, RangeVar *relation,
			const VacuumParams params, List *va_cols, bool in_outer_xact,
			BufferAccessStrategy bstrategy)
{
	Relation	onerel;
	int			elevel;
	AcquireSampleRowsFunc acquirefunc = NULL;
	BlockNumber relpages = 0;

	/* Select logging level */
	if (params.options & VACOPT_VERBOSE)
		elevel = INFO;
	else
		elevel = DEBUG2;

	/* Set up static variables */
	vac_strategy = bstrategy;

	/*
	 * Check for user-requested abort.
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Open the relation, getting ShareUpdateExclusiveLock to ensure that two
	 * ANALYZEs don't run on it concurrently.  (This also locks out a
	 * concurrent VACUUM, which doesn't matter much at the moment but might
	 * matter if we ever try to accumulate stats on dead tuples.) If the rel
	 * has been dropped since we last saw it, we don't need to process it.
	 *
	 * Make sure to generate only logs for ANALYZE in this case.
	 */
	onerel = vacuum_open_relation(relid, relation, params.options & ~(VACOPT_VACUUM),
								  params.log_min_duration >= 0,
								  ShareUpdateExclusiveLock);

	/* leave if relation could not be opened or locked */
	if (!onerel)
		return;

	/*
	 * Check if relation needs to be skipped based on privileges.  This check
	 * happens also when building the relation list to analyze for a manual
	 * operation, and needs to be done additionally here as ANALYZE could
	 * happen across multiple transactions where privileges could have changed
	 * in-between.  Make sure to generate only logs for ANALYZE in this case.
	 */
	if (!vacuum_is_permitted_for_relation(RelationGetRelid(onerel),
										  onerel->rd_rel,
										  params.options & ~VACOPT_VACUUM))
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * Silently ignore tables that are temp tables of other backends ---
	 * trying to analyze these is rather pointless, since their contents are
	 * probably not up-to-date on disk.  (We don't throw a warning here; it
	 * would just lead to chatter during a database-wide ANALYZE.)
	 */
	if (RELATION_IS_OTHER_TEMP(onerel))
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * We can ANALYZE any table except pg_statistic. See update_attstats
	 */
	if (RelationGetRelid(onerel) == StatisticRelationId)
	{
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * Check that it's of an analyzable relkind, and set up appropriately.
	 */
	if (onerel->rd_rel->relkind == RELKIND_RELATION ||
		onerel->rd_rel->relkind == RELKIND_MATVIEW)
	{
		/* Regular table, so we'll use the regular row acquisition function */
		acquirefunc = acquire_sample_rows;
		/* Also get regular table's size */
		relpages = RelationGetNumberOfBlocks(onerel);
	}
	else if (onerel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * For a foreign table, call the FDW's hook function to see whether it
		 * supports analysis.
		 */
		FdwRoutine *fdwroutine;
		bool		ok = false;

		fdwroutine = GetFdwRoutineForRelation(onerel, false);

		if (fdwroutine->AnalyzeForeignTable != NULL)
			ok = fdwroutine->AnalyzeForeignTable(onerel,
												 &acquirefunc,
												 &relpages);

		if (!ok)
		{
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- cannot analyze this foreign table",
							RelationGetRelationName(onerel))));
			relation_close(onerel, ShareUpdateExclusiveLock);
			return;
		}
	}
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * For partitioned tables, we want to do the recursive ANALYZE below.
		 */
	}
	else
	{
		/* No need for a WARNING if we already complained during VACUUM */
		if (!(params.options & VACOPT_VACUUM))
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- cannot analyze non-tables or special system tables",
							RelationGetRelationName(onerel))));
		relation_close(onerel, ShareUpdateExclusiveLock);
		return;
	}

	/*
	 * OK, let's do it.  First, initialize progress reporting.
	 */
	pgstat_progress_start_command(PROGRESS_COMMAND_ANALYZE,
								  RelationGetRelid(onerel));

	/*
	 * Do the normal non-recursive ANALYZE.  We can skip this for partitioned
	 * tables, which don't contain any rows.
	 */
	if (onerel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		do_analyze_rel(onerel, params, va_cols, acquirefunc,
					   relpages, false, in_outer_xact, elevel);

	/*
	 * If there are child tables, do recursive ANALYZE.
	 */
	if (onerel->rd_rel->relhassubclass)
		do_analyze_rel(onerel, params, va_cols, acquirefunc, relpages,
					   true, in_outer_xact, elevel);

	/*
	 * Close source relation now, but keep lock so that no one deletes it
	 * before we commit.  (If someone did, they'd fail to clean up the entries
	 * we made in pg_statistic.  Also, releasing the lock before commit would
	 * expose us to concurrent-update failures in update_attstats.)
	 */
	relation_close(onerel, NoLock);

	pgstat_progress_end_command();
}

/*
 *	do_analyze_rel() -- analyze one relation, recursively or not
 *
 * Note that "acquirefunc" is only relevant for the non-inherited case.
 * For the inherited case, acquire_inherited_sample_rows() determines the
 * appropriate acquirefunc for each child table.
 */
static void
do_analyze_rel(Relation onerel, const VacuumParams params,
			   List *va_cols, AcquireSampleRowsFunc acquirefunc,
			   BlockNumber relpages, bool inh, bool in_outer_xact,
			   int elevel)
{
	int			attr_cnt,
				tcnt,
				i,
				ind;
	Relation   *Irel;
	int			nindexes;
	bool		verbose,
				instrument,
				hasindex;
	VacAttrStats **vacattrstats;
	AnlIndexData *indexdata;
	int			targrows,
				numrows,
				minrows;
	double		totalrows,
				totaldeadrows;
	HeapTuple  *rows;
	PGRUsage	ru0;
	TimestampTz starttime = 0;
	MemoryContext caller_context;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	WalUsage	startwalusage = pgWalUsage;
	BufferUsage startbufferusage = pgBufferUsage;
	BufferUsage bufferusage;
	PgStat_Counter startreadtime = 0;
	PgStat_Counter startwritetime = 0;

	verbose = (params.options & VACOPT_VERBOSE) != 0;
	instrument = (verbose || (AmAutoVacuumWorkerProcess() &&
							  params.log_min_duration >= 0));
	if (inh)
		ereport(elevel,
				(errmsg("analyzing \"%s.%s\" inheritance tree",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
	else
		ereport(elevel,
				(errmsg("analyzing \"%s.%s\"",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));

	/*
	 * Set up a working context so that we can easily free whatever junk gets
	 * created.
	 */
	anl_context = AllocSetContextCreate(CurrentMemoryContext,
										"Analyze",
										ALLOCSET_DEFAULT_SIZES);
	caller_context = MemoryContextSwitchTo(anl_context);

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(onerel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/*
	 * When verbose or autovacuum logging is used, initialize a resource usage
	 * snapshot and optionally track I/O timing.
	 */
	if (instrument)
	{
		if (track_io_timing)
		{
			startreadtime = pgStatBlockReadTime;
			startwritetime = pgStatBlockWriteTime;
		}

		pg_rusage_init(&ru0);
	}

	/* Used for instrumentation and stats report */
	starttime = GetCurrentTimestamp();

	/*
	 * Determine which columns to analyze
	 *
	 * Note that system attributes are never analyzed, so we just reject them
	 * at the lookup stage.  We also reject duplicate column mentions.  (We
	 * could alternatively ignore duplicates, but analyzing a column twice
	 * won't work; we'd end up making a conflicting update in pg_statistic.)
	 */
	if (va_cols != NIL)
	{
		Bitmapset  *unique_cols = NULL;
		ListCell   *le;

		vacattrstats = (VacAttrStats **) palloc(list_length(va_cols) *
												sizeof(VacAttrStats *));
		tcnt = 0;
		foreach(le, va_cols)
		{
			char	   *col = strVal(lfirst(le));

			i = attnameAttNum(onerel, col, false);
			if (i == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" does not exist",
								col, RelationGetRelationName(onerel))));
			if (bms_is_member(i, unique_cols))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" of relation \"%s\" appears more than once",
								col, RelationGetRelationName(onerel))));
			unique_cols = bms_add_member(unique_cols, i);

			vacattrstats[tcnt] = examine_attribute(onerel, i, NULL);
			if (vacattrstats[tcnt] != NULL)
				tcnt++;
		}
		attr_cnt = tcnt;
	}
	else
	{
		attr_cnt = onerel->rd_att->natts;
		vacattrstats = (VacAttrStats **)
			palloc(attr_cnt * sizeof(VacAttrStats *));
		tcnt = 0;
		for (i = 1; i <= attr_cnt; i++)
		{
			vacattrstats[tcnt] = examine_attribute(onerel, i, NULL);
			if (vacattrstats[tcnt] != NULL)
				tcnt++;
		}
		attr_cnt = tcnt;
	}

	/*
	 * Open all indexes of the relation, and see if there are any analyzable
	 * columns in the indexes.  We do not analyze index columns if there was
	 * an explicit column list in the ANALYZE command, however.
	 *
	 * If we are doing a recursive scan, we don't want to touch the parent's
	 * indexes at all.  If we're processing a partitioned table, we need to
	 * know if there are any indexes, but we don't want to process them.
	 */
	if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *idxs = RelationGetIndexList(onerel);

		Irel = NULL;
		nindexes = 0;
		hasindex = idxs != NIL;
		list_free(idxs);
	}
	else if (!inh)
	{
		vac_open_indexes(onerel, AccessShareLock, &nindexes, &Irel);
		hasindex = nindexes > 0;
	}
	else
	{
		Irel = NULL;
		nindexes = 0;
		hasindex = false;
	}
	indexdata = NULL;
	if (nindexes > 0)
	{
		indexdata = (AnlIndexData *) palloc0(nindexes * sizeof(AnlIndexData));
		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];
			IndexInfo  *indexInfo;

			thisdata->indexInfo = indexInfo = BuildIndexInfo(Irel[ind]);
			thisdata->tupleFract = 1.0; /* fix later if partial */
			if (indexInfo->ii_Expressions != NIL && va_cols == NIL)
			{
				ListCell   *indexpr_item = list_head(indexInfo->ii_Expressions);

				thisdata->vacattrstats = (VacAttrStats **)
					palloc(indexInfo->ii_NumIndexAttrs * sizeof(VacAttrStats *));
				tcnt = 0;
				for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
				{
					int			keycol = indexInfo->ii_IndexAttrNumbers[i];

					if (keycol == 0)
					{
						/* Found an index expression */
						Node	   *indexkey;

						if (indexpr_item == NULL)	/* shouldn't happen */
							elog(ERROR, "too few entries in indexprs list");
						indexkey = (Node *) lfirst(indexpr_item);
						indexpr_item = lnext(indexInfo->ii_Expressions,
											 indexpr_item);
						thisdata->vacattrstats[tcnt] =
							examine_attribute(Irel[ind], i + 1, indexkey);
						if (thisdata->vacattrstats[tcnt] != NULL)
							tcnt++;
					}
				}
				thisdata->attr_cnt = tcnt;
			}
		}
	}

	/*
	 * Determine how many rows we need to sample, using the worst case from
	 * all analyzable columns.  We use a lower bound of 100 rows to avoid
	 * possible overflow in Vitter's algorithm.  (Note: that will also be the
	 * target in the corner case where there are no analyzable columns.)
	 */
	targrows = 100;
	for (i = 0; i < attr_cnt; i++)
	{
		if (targrows < vacattrstats[i]->minrows)
			targrows = vacattrstats[i]->minrows;
	}
	for (ind = 0; ind < nindexes; ind++)
	{
		AnlIndexData *thisdata = &indexdata[ind];

		for (i = 0; i < thisdata->attr_cnt; i++)
		{
			if (targrows < thisdata->vacattrstats[i]->minrows)
				targrows = thisdata->vacattrstats[i]->minrows;
		}
	}

	/*
	 * Look at extended statistics objects too, as those may define custom
	 * statistics target. So we may need to sample more rows and then build
	 * the statistics with enough detail.
	 */
	minrows = ComputeExtStatisticsRows(onerel, attr_cnt, vacattrstats);

	if (targrows < minrows)
		targrows = minrows;

	/*
	 * Acquire the sample rows
	 */
	rows = (HeapTuple *) palloc(targrows * sizeof(HeapTuple));
	pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
								 inh ? PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS_INH :
								 PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS);
	if (inh)
		numrows = acquire_inherited_sample_rows(onerel, elevel,
												rows, targrows,
												&totalrows, &totaldeadrows);
	else
		numrows = (*acquirefunc) (onerel, elevel,
								  rows, targrows,
								  &totalrows, &totaldeadrows);

	/*
	 * Compute the statistics.  Temporary results during the calculations for
	 * each column are stored in a child context.  The calc routines are
	 * responsible to make sure that whatever they store into the VacAttrStats
	 * structure is allocated in anl_context.
	 */
	if (numrows > 0)
	{
		MemoryContext col_context,
					old_context;

		pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
									 PROGRESS_ANALYZE_PHASE_COMPUTE_STATS);

		col_context = AllocSetContextCreate(anl_context,
											"Analyze Column",
											ALLOCSET_DEFAULT_SIZES);
		old_context = MemoryContextSwitchTo(col_context);

		for (i = 0; i < attr_cnt; i++)
		{
			VacAttrStats *stats = vacattrstats[i];
			AttributeOpts *aopt;

			stats->rows = rows;
			stats->tupDesc = onerel->rd_att;
			stats->compute_stats(stats,
								 std_fetch_func,
								 numrows,
								 totalrows);

			/*
			 * If the appropriate flavor of the n_distinct option is
			 * specified, override with the corresponding value.
			 */
			aopt = get_attribute_options(onerel->rd_id, stats->tupattnum);
			if (aopt != NULL)
			{
				float8		n_distinct;

				n_distinct = inh ? aopt->n_distinct_inherited : aopt->n_distinct;
				if (n_distinct != 0.0)
					stats->stadistinct = n_distinct;
			}

			MemoryContextReset(col_context);
		}

		if (nindexes > 0)
			compute_index_stats(onerel, totalrows,
								indexdata, nindexes,
								rows, numrows,
								col_context);

		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(col_context);

		/*
		 * Emit the completed stats rows into pg_statistic, replacing any
		 * previous statistics for the target columns.  (If there are stats in
		 * pg_statistic for columns we didn't process, we leave them alone.)
		 */
		update_attstats(RelationGetRelid(onerel), inh,
						attr_cnt, vacattrstats);

		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];

			update_attstats(RelationGetRelid(Irel[ind]), false,
							thisdata->attr_cnt, thisdata->vacattrstats);
		}

		/* Build extended statistics (if there are any). */
		BuildRelationExtStatistics(onerel, inh, totalrows, numrows, rows,
								   attr_cnt, vacattrstats);
	}

	pgstat_progress_update_param(PROGRESS_ANALYZE_PHASE,
								 PROGRESS_ANALYZE_PHASE_FINALIZE_ANALYZE);

	/*
	 * Update pages/tuples stats in pg_class ... but not if we're doing
	 * inherited stats.
	 *
	 * We assume that VACUUM hasn't set pg_class.reltuples already, even
	 * during a VACUUM ANALYZE.  Although VACUUM often updates pg_class,
	 * exceptions exist.  A "VACUUM (ANALYZE, INDEX_CLEANUP OFF)" command will
	 * never update pg_class entries for index relations.  It's also possible
	 * that an individual index's pg_class entry won't be updated during
	 * VACUUM if the index AM returns NULL from its amvacuumcleanup() routine.
	 */
	if (!inh)
	{
		BlockNumber relallvisible = 0;
		BlockNumber relallfrozen = 0;

		if (RELKIND_HAS_STORAGE(onerel->rd_rel->relkind))
			visibilitymap_count(onerel, &relallvisible, &relallfrozen);

		/*
		 * Update pg_class for table relation.  CCI first, in case acquirefunc
		 * updated pg_class.
		 */
		CommandCounterIncrement();
		vac_update_relstats(onerel,
							relpages,
							totalrows,
							relallvisible,
							relallfrozen,
							hasindex,
							InvalidTransactionId,
							InvalidMultiXactId,
							NULL, NULL,
							in_outer_xact);

		/* Same for indexes */
		for (ind = 0; ind < nindexes; ind++)
		{
			AnlIndexData *thisdata = &indexdata[ind];
			double		totalindexrows;

			totalindexrows = ceil(thisdata->tupleFract * totalrows);
			vac_update_relstats(Irel[ind],
								RelationGetNumberOfBlocks(Irel[ind]),
								totalindexrows,
								0, 0,
								false,
								InvalidTransactionId,
								InvalidMultiXactId,
								NULL, NULL,
								in_outer_xact);
		}
	}
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * Partitioned tables don't have storage, so we don't set any fields
		 * in their pg_class entries except for reltuples and relhasindex.
		 */
		CommandCounterIncrement();
		vac_update_relstats(onerel, -1, totalrows,
							0, 0, hasindex, InvalidTransactionId,
							InvalidMultiXactId,
							NULL, NULL,
							in_outer_xact);
	}

	/*
	 * Now report ANALYZE to the cumulative stats system.  For regular tables,
	 * we do it only if not doing inherited stats.  For partitioned tables, we
	 * only do it for inherited stats. (We're never called for not-inherited
	 * stats on partitioned tables anyway.)
	 *
	 * Reset the changes_since_analyze counter only if we analyzed all
	 * columns; otherwise, there is still work for auto-analyze to do.
	 */
	if (!inh)
		pgstat_report_analyze(onerel, totalrows, totaldeadrows,
							  (va_cols == NIL), starttime);
	else if (onerel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		pgstat_report_analyze(onerel, 0, 0, (va_cols == NIL), starttime);

	/*
	 * If this isn't part of VACUUM ANALYZE, let index AMs do cleanup.
	 *
	 * Note that most index AMs perform a no-op as a matter of policy for
	 * amvacuumcleanup() when called in ANALYZE-only mode.  The only exception
	 * among core index AMs is GIN/ginvacuumcleanup().
	 */
	if (!(params.options & VACOPT_VACUUM))
	{
		for (ind = 0; ind < nindexes; ind++)
		{
			IndexBulkDeleteResult *stats;
			IndexVacuumInfo ivinfo;

			ivinfo.index = Irel[ind];
			ivinfo.heaprel = onerel;
			ivinfo.analyze_only = true;
			ivinfo.estimated_count = true;
			ivinfo.message_level = elevel;
			ivinfo.num_heap_tuples = onerel->rd_rel->reltuples;
			ivinfo.strategy = vac_strategy;

			stats = index_vacuum_cleanup(&ivinfo, NULL);

			if (stats)
				pfree(stats);
		}
	}

	/* Done with indexes */
	vac_close_indexes(nindexes, Irel, NoLock);

	/* Log the action if appropriate */
	if (instrument)
	{
		TimestampTz endtime = GetCurrentTimestamp();

		if (verbose || params.log_min_duration == 0 ||
			TimestampDifferenceExceeds(starttime, endtime,
									   params.log_min_duration))
		{
			long		delay_in_ms;
			WalUsage	walusage;
			double		read_rate = 0;
			double		write_rate = 0;
			char	   *msgfmt;
			StringInfoData buf;
			int64		total_blks_hit;
			int64		total_blks_read;
			int64		total_blks_dirtied;

			memset(&bufferusage, 0, sizeof(BufferUsage));
			BufferUsageAccumDiff(&bufferusage, &pgBufferUsage, &startbufferusage);
			memset(&walusage, 0, sizeof(WalUsage));
			WalUsageAccumDiff(&walusage, &pgWalUsage, &startwalusage);

			total_blks_hit = bufferusage.shared_blks_hit +
				bufferusage.local_blks_hit;
			total_blks_read = bufferusage.shared_blks_read +
				bufferusage.local_blks_read;
			total_blks_dirtied = bufferusage.shared_blks_dirtied +
				bufferusage.local_blks_dirtied;

			/*
			 * We do not expect an analyze to take > 25 days and it simplifies
			 * things a bit to use TimestampDifferenceMilliseconds.
			 */
			delay_in_ms = TimestampDifferenceMilliseconds(starttime, endtime);

			/*
			 * Note that we are reporting these read/write rates in the same
			 * manner as VACUUM does, which means that while the 'average read
			 * rate' here actually corresponds to page misses and resulting
			 * reads which are also picked up by track_io_timing, if enabled,
			 * the 'average write rate' is actually talking about the rate of
			 * pages being dirtied, not being written out, so it's typical to
			 * have a non-zero 'avg write rate' while I/O timings only reports
			 * reads.
			 *
			 * It's not clear that an ANALYZE will ever result in
			 * FlushBuffer() being called, but we track and support reporting
			 * on I/O write time in case that changes as it's practically free
			 * to do so anyway.
			 */

			if (delay_in_ms > 0)
			{
				read_rate = (double) BLCKSZ * total_blks_read /
					(1024 * 1024) / (delay_in_ms / 1000.0);
				write_rate = (double) BLCKSZ * total_blks_dirtied /
					(1024 * 1024) / (delay_in_ms / 1000.0);
			}

			/*
			 * We split this up so we don't emit empty I/O timing values when
			 * track_io_timing isn't enabled.
			 */

			initStringInfo(&buf);

			if (AmAutoVacuumWorkerProcess())
				msgfmt = _("automatic analyze of table \"%s.%s.%s\"\n");
			else
				msgfmt = _("finished analyzing table \"%s.%s.%s\"\n");

			appendStringInfo(&buf, msgfmt,
							 get_database_name(MyDatabaseId),
							 get_namespace_name(RelationGetNamespace(onerel)),
							 RelationGetRelationName(onerel));
			if (track_cost_delay_timing)
			{
				/*
				 * We bypass the changecount mechanism because this value is
				 * only updated by the calling process.
				 */
				appendStringInfo(&buf, _("delay time: %.3f ms\n"),
								 (double) MyBEEntry->st_progress_param[PROGRESS_ANALYZE_DELAY_TIME] / 1000000.0);
			}
			if (track_io_timing)
			{
				double		read_ms = (double) (pgStatBlockReadTime - startreadtime) / 1000;
				double		write_ms = (double) (pgStatBlockWriteTime - startwritetime) / 1000;

				appendStringInfo(&buf, _("I/O timings: read: %.3f ms, write: %.3f ms\n"),
								 read_ms, write_ms);
			}
			appendStringInfo(&buf, _("avg read rate: %.3f MB/s, avg write rate: %.3f MB/s\n"),
							 read_rate, write_rate);
			appendStringInfo(&buf, _("buffer usage: %" PRId64 " hits, %" PRId64 " reads, %" PRId64 " dirtied\n"),
							 total_blks_hit,
							 total_blks_read,
							 total_blks_dirtied);
			appendStringInfo(&buf,
							 _("WAL usage: %" PRId64 " records, %" PRId64 " full page images, %" PRIu64 " bytes, %" PRId64 " buffers full\n"),
							 walusage.wal_records,
							 walusage.wal_fpi,
							 walusage.wal_bytes,
							 walusage.wal_buffers_full);
			appendStringInfo(&buf, _("system usage: %s"), pg_rusage_show(&ru0));

			ereport(verbose ? INFO : LOG,
					(errmsg_internal("%s", buf.data)));

			pfree(buf.data);
		}
	}

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* Restore current context and release memory */
	MemoryContextSwitchTo(caller_context);
	MemoryContextDelete(anl_context);
	anl_context = NULL;
}

/*
 * Compute statistics about indexes of a relation
 */
static void
compute_index_stats(Relation onerel, double totalrows,
					AnlIndexData *indexdata, int nindexes,
					HeapTuple *rows, int numrows,
					MemoryContext col_context)
{
	MemoryContext ind_context,
				old_context;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			ind,
				i;

	ind_context = AllocSetContextCreate(anl_context,
										"Analyze Index",
										ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(ind_context);

	for (ind = 0; ind < nindexes; ind++)
	{
		AnlIndexData *thisdata = &indexdata[ind];
		IndexInfo  *indexInfo = thisdata->indexInfo;
		int			attr_cnt = thisdata->attr_cnt;
		TupleTableSlot *slot;
		EState	   *estate;
		ExprContext *econtext;
		ExprState  *predicate;
		Datum	   *exprvals;
		bool	   *exprnulls;
		int			numindexrows,
					tcnt,
					rowno;
		double		totalindexrows;

		/* Ignore index if no columns to analyze and not partial */
		if (attr_cnt == 0 && indexInfo->ii_Predicate == NIL)
			continue;

		/*
		 * Need an EState for evaluation of index expressions and
		 * partial-index predicates.  Create it in the per-index context to be
		 * sure it gets cleaned up at the bottom of the loop.
		 */
		estate = CreateExecutorState();
		econtext = GetPerTupleExprContext(estate);
		/* Need a slot to hold the current heap tuple, too */
		slot = MakeSingleTupleTableSlot(RelationGetDescr(onerel),
										&TTSOpsHeapTuple);

		/* Arrange for econtext's scan tuple to be the tuple under test */
		econtext->ecxt_scantuple = slot;

		/* Set up execution state for predicate. */
		predicate = ExecPrepareQual(indexInfo->ii_Predicate, estate);

		/* Compute and save index expression values */
		exprvals = (Datum *) palloc(numrows * attr_cnt * sizeof(Datum));
		exprnulls = (bool *) palloc(numrows * attr_cnt * sizeof(bool));
		numindexrows = 0;
		tcnt = 0;
		for (rowno = 0; rowno < numrows; rowno++)
		{
			HeapTuple	heapTuple = rows[rowno];

			vacuum_delay_point(true);

			/*
			 * Reset the per-tuple context each time, to reclaim any cruft
			 * left behind by evaluating the predicate or index expressions.
			 */
			ResetExprContext(econtext);

			/* Set up for predicate or expression evaluation */
			ExecStoreHeapTuple(heapTuple, slot, false);

			/* If index is partial, check predicate */
			if (predicate != NULL)
			{
				if (!ExecQual(predicate, econtext))
					continue;
			}
			numindexrows++;

			if (attr_cnt > 0)
			{
				/*
				 * Evaluate the index row to compute expression values. We
				 * could do this by hand, but FormIndexDatum is convenient.
				 */
				FormIndexDatum(indexInfo,
							   slot,
							   estate,
							   values,
							   isnull);

				/*
				 * Save just the columns we care about.  We copy the values
				 * into ind_context from the estate's per-tuple context.
				 */
				for (i = 0; i < attr_cnt; i++)
				{
					VacAttrStats *stats = thisdata->vacattrstats[i];
					int			attnum = stats->tupattnum;

					if (isnull[attnum - 1])
					{
						exprvals[tcnt] = (Datum) 0;
						exprnulls[tcnt] = true;
					}
					else
					{
						exprvals[tcnt] = datumCopy(values[attnum - 1],
												   stats->attrtype->typbyval,
												   stats->attrtype->typlen);
						exprnulls[tcnt] = false;
					}
					tcnt++;
				}
			}
		}

		/*
		 * Having counted the number of rows that pass the predicate in the
		 * sample, we can estimate the total number of rows in the index.
		 */
		thisdata->tupleFract = (double) numindexrows / (double) numrows;
		totalindexrows = ceil(thisdata->tupleFract * totalrows);

		/*
		 * Now we can compute the statistics for the expression columns.
		 */
		if (numindexrows > 0)
		{
			MemoryContextSwitchTo(col_context);
			for (i = 0; i < attr_cnt; i++)
			{
				VacAttrStats *stats = thisdata->vacattrstats[i];

				stats->exprvals = exprvals + i;
				stats->exprnulls = exprnulls + i;
				stats->rowstride = attr_cnt;
				stats->compute_stats(stats,
									 ind_fetch_func,
									 numindexrows,
									 totalindexrows);

				MemoryContextReset(col_context);
			}
		}

		/* And clean up */
		MemoryContextSwitchTo(ind_context);

		ExecDropSingleTupleTableSlot(slot);
		FreeExecutorState(estate);
		MemoryContextReset(ind_context);
	}

	MemoryContextSwitchTo(old_context);
	MemoryContextDelete(ind_context);
}

/*
 * examine_attribute -- pre-analysis of a single column
 *
 * Determine whether the column is analyzable; if so, create and initialize
 * a VacAttrStats struct for it.  If not, return NULL.
 *
 * If index_expr isn't NULL, then we're trying to analyze an expression index,
 * and index_expr is the expression tree representing the column's data.
 */
static VacAttrStats *
examine_attribute(Relation onerel, int attnum, Node *index_expr)
{
	Form_pg_attribute attr = TupleDescAttr(onerel->rd_att, attnum - 1);
	int			attstattarget;
	HeapTuple	atttuple;
	Datum		dat;
	bool		isnull;
	HeapTuple	typtuple;
	VacAttrStats *stats;
	int			i;
	bool		ok;

	/* Never analyze dropped columns */
	if (attr->attisdropped)
		return NULL;

	/* Don't analyze virtual generated columns */
	if (attr->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
		return NULL;

	/*
	 * Get attstattarget value.  Set to -1 if null.  (Analyze functions expect
	 * -1 to mean use default_statistics_target; see for example
	 * std_typanalyze.)
	 */
	atttuple = SearchSysCache2(ATTNUM, ObjectIdGetDatum(RelationGetRelid(onerel)), Int16GetDatum(attnum));
	if (!HeapTupleIsValid(atttuple))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(onerel));
	dat = SysCacheGetAttr(ATTNUM, atttuple, Anum_pg_attribute_attstattarget, &isnull);
	attstattarget = isnull ? -1 : DatumGetInt16(dat);
	ReleaseSysCache(atttuple);

	/* Don't analyze column if user has specified not to */
	if (attstattarget == 0)
		return NULL;

	/*
	 * Create the VacAttrStats struct.
	 */
	stats = (VacAttrStats *) palloc0(sizeof(VacAttrStats));
	stats->attstattarget = attstattarget;

	/*
	 * When analyzing an expression index, believe the expression tree's type
	 * not the column datatype --- the latter might be the opckeytype storage
	 * type of the opclass, which is not interesting for our purposes.  (Note:
	 * if we did anything with non-expression index columns, we'd need to
	 * figure out where to get the correct type info from, but for now that's
	 * not a problem.)	It's not clear whether anyone will care about the
	 * typmod, but we store that too just in case.
	 */
	if (index_expr)
	{
		stats->attrtypid = exprType(index_expr);
		stats->attrtypmod = exprTypmod(index_expr);

		/*
		 * If a collation has been specified for the index column, use that in
		 * preference to anything else; but if not, fall back to whatever we
		 * can get from the expression.
		 */
		if (OidIsValid(onerel->rd_indcollation[attnum - 1]))
			stats->attrcollid = onerel->rd_indcollation[attnum - 1];
		else
			stats->attrcollid = exprCollation(index_expr);
	}
	else
	{
		stats->attrtypid = attr->atttypid;
		stats->attrtypmod = attr->atttypmod;
		stats->attrcollid = attr->attcollation;
	}

	typtuple = SearchSysCacheCopy1(TYPEOID,
								   ObjectIdGetDatum(stats->attrtypid));
	if (!HeapTupleIsValid(typtuple))
		elog(ERROR, "cache lookup failed for type %u", stats->attrtypid);
	stats->attrtype = (Form_pg_type) GETSTRUCT(typtuple);
	stats->anl_context = anl_context;
	stats->tupattnum = attnum;

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
 * Read stream callback returning the next BlockNumber as chosen by the
 * BlockSampling algorithm.
 */
static BlockNumber
block_sampling_read_stream_next(ReadStream *stream,
								void *callback_private_data,
								void *per_buffer_data)
{
	BlockSamplerData *bs = callback_private_data;

	return BlockSampler_HasMore(bs) ? BlockSampler_Next(bs) : InvalidBlockNumber;
}

/*
 * acquire_sample_rows -- acquire a random sample of rows from the table
 *
 * Selected rows are returned in the caller-allocated array rows[], which
 * must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also estimate the total numbers of live and dead rows in the table,
 * and return them into *totalrows and *totaldeadrows, respectively.
 *
 * The returned list of tuples is in order by physical position in the table.
 * (We will rely on this later to derive correlation estimates.)
 *
 * As of May 2004 we use a new two-stage method:  Stage one selects up
 * to targrows random blocks (or all blocks, if there aren't so many).
 * Stage two scans these blocks and uses the Vitter algorithm to create
 * a random sample of targrows rows (or less, if there are less in the
 * sample of blocks).  The two stages are executed simultaneously: each
 * block is processed as soon as stage one returns its number and while
 * the rows are read stage two controls which ones are to be inserted
 * into the sample.
 *
 * Although every row has an equal chance of ending up in the final
 * sample, this sampling method is not perfect: not every possible
 * sample has an equal chance of being selected.  For large relations
 * the number of different blocks represented by the sample tends to be
 * too small.  We can live with that for now.  Improvements are welcome.
 *
 * An important property of this sampling method is that because we do
 * look at a statistically unbiased set of blocks, we should get
 * unbiased estimates of the average numbers of live and dead rows per
 * block.  The previous sampling method put too much credence in the row
 * density near the start of the table.
 */
static int
acquire_sample_rows(Relation onerel, int elevel,
					HeapTuple *rows, int targrows,
					double *totalrows, double *totaldeadrows)
{
	int			numrows = 0;	/* # rows now in reservoir */
	double		samplerows = 0; /* total # rows collected */
	double		liverows = 0;	/* # live rows seen */
	double		deadrows = 0;	/* # dead rows seen */
	double		rowstoskip = -1;	/* -1 means not set yet */
	uint32		randseed;		/* Seed for block sampler(s) */
	BlockNumber totalblocks;
	TransactionId OldestXmin;
	BlockSamplerData bs;
	ReservoirStateData rstate;
	TupleTableSlot *slot;
	TableScanDesc scan;
	BlockNumber nblocks;
	BlockNumber blksdone = 0;
	ReadStream *stream;

	Assert(targrows > 0);

	totalblocks = RelationGetNumberOfBlocks(onerel);

	/* Need a cutoff xmin for HeapTupleSatisfiesVacuum */
	OldestXmin = GetOldestNonRemovableTransactionId(onerel);

	/* Prepare for sampling block numbers */
	randseed = pg_prng_uint32(&pg_global_prng_state);
	nblocks = BlockSampler_Init(&bs, totalblocks, targrows, randseed);

	/* Report sampling block numbers */
	pgstat_progress_update_param(PROGRESS_ANALYZE_BLOCKS_TOTAL,
								 nblocks);

	/* Prepare for sampling rows */
	reservoir_init_selection_state(&rstate, targrows);

	scan = table_beginscan_analyze(onerel);
	slot = table_slot_create(onerel, NULL);

	/*
	 * It is safe to use batching, as block_sampling_read_stream_next never
	 * blocks.
	 */
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE |
										READ_STREAM_USE_BATCHING,
										vac_strategy,
										scan->rs_rd,
										MAIN_FORKNUM,
										block_sampling_read_stream_next,
										&bs,
										0);

	/* Outer loop over blocks to sample */
	while (table_scan_analyze_next_block(scan, stream))
	{
		vacuum_delay_point(true);

		while (table_scan_analyze_next_tuple(scan, OldestXmin, &liverows, &deadrows, slot))
		{
			/*
			 * The first targrows sample rows are simply copied into the
			 * reservoir. Then we start replacing tuples in the sample until
			 * we reach the end of the relation.  This algorithm is from Jeff
			 * Vitter's paper (see full citation in utils/misc/sampling.c). It
			 * works by repeatedly computing the number of tuples to skip
			 * before selecting a tuple, which replaces a randomly chosen
			 * element of the reservoir (current set of tuples).  At all times
			 * the reservoir is a true random sample of the tuples we've
			 * passed over so far, so when we fall off the end of the relation
			 * we're done.
			 */
			if (numrows < targrows)
				rows[numrows++] = ExecCopySlotHeapTuple(slot);
			else
			{
				/*
				 * t in Vitter's paper is the number of records already
				 * processed.  If we need to compute a new S value, we must
				 * use the not-yet-incremented value of samplerows as t.
				 */
				if (rowstoskip < 0)
					rowstoskip = reservoir_get_next_S(&rstate, samplerows, targrows);

				if (rowstoskip <= 0)
				{
					/*
					 * Found a suitable tuple, so save it, replacing one old
					 * tuple at random
					 */
					int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

					Assert(k >= 0 && k < targrows);
					heap_freetuple(rows[k]);
					rows[k] = ExecCopySlotHeapTuple(slot);
				}

				rowstoskip -= 1;
			}

			samplerows += 1;
		}

		pgstat_progress_update_param(PROGRESS_ANALYZE_BLOCKS_DONE,
									 ++blksdone);
	}

	read_stream_end(stream);

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

	/*
	 * If we didn't find as many tuples as we wanted then we're done. No sort
	 * is needed, since they're already in order.
	 *
	 * Otherwise we need to sort the collected tuples by position
	 * (itempointer). It's not worth worrying about corner cases where the
	 * tuples are already sorted.
	 */
	if (numrows == targrows)
		qsort_interruptible(rows, numrows, sizeof(HeapTuple),
							compare_rows, NULL);

	/*
	 * Estimate total numbers of live and dead rows in relation, extrapolating
	 * on the assumption that the average tuple density in pages we didn't
	 * scan is the same as in the pages we did scan.  Since what we scanned is
	 * a random sample of the pages in the relation, this should be a good
	 * assumption.
	 */
	if (bs.m > 0)
	{
		*totalrows = floor((liverows / bs.m) * totalblocks + 0.5);
		*totaldeadrows = floor((deadrows / bs.m) * totalblocks + 0.5);
	}
	else
	{
		*totalrows = 0.0;
		*totaldeadrows = 0.0;
	}

	/*
	 * Emit some interesting relation info
	 */
	ereport(elevel,
			(errmsg("\"%s\": scanned %d of %u pages, "
					"containing %.0f live rows and %.0f dead rows; "
					"%d rows in sample, %.0f estimated total rows",
					RelationGetRelationName(onerel),
					bs.m, totalblocks,
					liverows, deadrows,
					numrows, *totalrows)));

	return numrows;
}

/*
 * Comparator for sorting rows[] array
 */
static int
compare_rows(const void *a, const void *b, void *arg)
{
	HeapTuple	ha = *(const HeapTuple *) a;
	HeapTuple	hb = *(const HeapTuple *) b;
	BlockNumber ba = ItemPointerGetBlockNumber(&ha->t_self);
	OffsetNumber oa = ItemPointerGetOffsetNumber(&ha->t_self);
	BlockNumber bb = ItemPointerGetBlockNumber(&hb->t_self);
	OffsetNumber ob = ItemPointerGetOffsetNumber(&hb->t_self);

	if (ba < bb)
		return -1;
	if (ba > bb)
		return 1;
	if (oa < ob)
		return -1;
	if (oa > ob)
		return 1;
	return 0;
}


/*
 * acquire_inherited_sample_rows -- acquire sample rows from inheritance tree
 *
 * This has the same API as acquire_sample_rows, except that rows are
 * collected from all inheritance children as well as the specified table.
 * We fail and return zero if there are no inheritance children, or if all
 * children are foreign tables that don't support ANALYZE.
 */
static int
acquire_inherited_sample_rows(Relation onerel, int elevel,
							  HeapTuple *rows, int targrows,
							  double *totalrows, double *totaldeadrows)
{
	List	   *tableOIDs;
	Relation   *rels;
	AcquireSampleRowsFunc *acquirefuncs;
	double	   *relblocks;
	double		totalblocks;
	int			numrows,
				nrels,
				i;
	ListCell   *lc;
	bool		has_child;

	/* Initialize output parameters to zero now, in case we exit early */
	*totalrows = 0;
	*totaldeadrows = 0;

	/*
	 * Find all members of inheritance set.  We only need AccessShareLock on
	 * the children.
	 */
	tableOIDs =
		find_all_inheritors(RelationGetRelid(onerel), AccessShareLock, NULL);

	/*
	 * Check that there's at least one descendant, else fail.  This could
	 * happen despite analyze_rel's relhassubclass check, if table once had a
	 * child but no longer does.  In that case, we can clear the
	 * relhassubclass field so as not to make the same mistake again later.
	 * (This is safe because we hold ShareUpdateExclusiveLock.)
	 */
	if (list_length(tableOIDs) < 2)
	{
		/* CCI because we already updated the pg_class row in this command */
		CommandCounterIncrement();
		SetRelationHasSubclass(RelationGetRelid(onerel), false);
		ereport(elevel,
				(errmsg("skipping analyze of \"%s.%s\" inheritance tree --- this inheritance tree contains no child tables",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
		return 0;
	}

	/*
	 * Identify acquirefuncs to use, and count blocks in all the relations.
	 * The result could overflow BlockNumber, so we use double arithmetic.
	 */
	rels = (Relation *) palloc(list_length(tableOIDs) * sizeof(Relation));
	acquirefuncs = (AcquireSampleRowsFunc *)
		palloc(list_length(tableOIDs) * sizeof(AcquireSampleRowsFunc));
	relblocks = (double *) palloc(list_length(tableOIDs) * sizeof(double));
	totalblocks = 0;
	nrels = 0;
	has_child = false;
	foreach(lc, tableOIDs)
	{
		Oid			childOID = lfirst_oid(lc);
		Relation	childrel;
		AcquireSampleRowsFunc acquirefunc = NULL;
		BlockNumber relpages = 0;

		/* We already got the needed lock */
		childrel = table_open(childOID, NoLock);

		/* Ignore if temp table of another backend */
		if (RELATION_IS_OTHER_TEMP(childrel))
		{
			/* ... but release the lock on it */
			Assert(childrel != onerel);
			table_close(childrel, AccessShareLock);
			continue;
		}

		/* Check table type (MATVIEW can't happen, but might as well allow) */
		if (childrel->rd_rel->relkind == RELKIND_RELATION ||
			childrel->rd_rel->relkind == RELKIND_MATVIEW)
		{
			/* Regular table, so use the regular row acquisition function */
			acquirefunc = acquire_sample_rows;
			relpages = RelationGetNumberOfBlocks(childrel);
		}
		else if (childrel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			/*
			 * For a foreign table, call the FDW's hook function to see
			 * whether it supports analysis.
			 */
			FdwRoutine *fdwroutine;
			bool		ok = false;

			fdwroutine = GetFdwRoutineForRelation(childrel, false);

			if (fdwroutine->AnalyzeForeignTable != NULL)
				ok = fdwroutine->AnalyzeForeignTable(childrel,
													 &acquirefunc,
													 &relpages);

			if (!ok)
			{
				/* ignore, but release the lock on it */
				Assert(childrel != onerel);
				table_close(childrel, AccessShareLock);
				continue;
			}
		}
		else
		{
			/*
			 * ignore, but release the lock on it.  don't try to unlock the
			 * passed-in relation
			 */
			Assert(childrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			if (childrel != onerel)
				table_close(childrel, AccessShareLock);
			else
				table_close(childrel, NoLock);
			continue;
		}

		/* OK, we'll process this child */
		has_child = true;
		rels[nrels] = childrel;
		acquirefuncs[nrels] = acquirefunc;
		relblocks[nrels] = (double) relpages;
		totalblocks += (double) relpages;
		nrels++;
	}

	/*
	 * If we don't have at least one child table to consider, fail.  If the
	 * relation is a partitioned table, it's not counted as a child table.
	 */
	if (!has_child)
	{
		ereport(elevel,
				(errmsg("skipping analyze of \"%s.%s\" inheritance tree --- this inheritance tree contains no analyzable child tables",
						get_namespace_name(RelationGetNamespace(onerel)),
						RelationGetRelationName(onerel))));
		return 0;
	}

	/*
	 * Now sample rows from each relation, proportionally to its fraction of
	 * the total block count.  (This might be less than desirable if the child
	 * rels have radically different free-space percentages, but it's not
	 * clear that it's worth working harder.)
	 */
	pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_TOTAL,
								 nrels);
	numrows = 0;
	for (i = 0; i < nrels; i++)
	{
		Relation	childrel = rels[i];
		AcquireSampleRowsFunc acquirefunc = acquirefuncs[i];
		double		childblocks = relblocks[i];

		/*
		 * Report progress.  The sampling function will normally report blocks
		 * done/total, but we need to reset them to 0 here, so that they don't
		 * show an old value until that.
		 */
		{
			const int	progress_index[] = {
				PROGRESS_ANALYZE_CURRENT_CHILD_TABLE_RELID,
				PROGRESS_ANALYZE_BLOCKS_DONE,
				PROGRESS_ANALYZE_BLOCKS_TOTAL
			};
			const int64 progress_vals[] = {
				RelationGetRelid(childrel),
				0,
				0,
			};

			pgstat_progress_update_multi_param(3, progress_index, progress_vals);
		}

		if (childblocks > 0)
		{
			int			childtargrows;

			childtargrows = (int) rint(targrows * childblocks / totalblocks);
			/* Make sure we don't overrun due to roundoff error */
			childtargrows = Min(childtargrows, targrows - numrows);
			if (childtargrows > 0)
			{
				int			childrows;
				double		trows,
							tdrows;

				/* Fetch a random sample of the child's rows */
				childrows = (*acquirefunc) (childrel, elevel,
											rows + numrows, childtargrows,
											&trows, &tdrows);

				/* We may need to convert from child's rowtype to parent's */
				if (childrows > 0 &&
					!equalRowTypes(RelationGetDescr(childrel),
								   RelationGetDescr(onerel)))
				{
					TupleConversionMap *map;

					map = convert_tuples_by_name(RelationGetDescr(childrel),
												 RelationGetDescr(onerel));
					if (map != NULL)
					{
						int			j;

						for (j = 0; j < childrows; j++)
						{
							HeapTuple	newtup;

							newtup = execute_attr_map_tuple(rows[numrows + j], map);
							heap_freetuple(rows[numrows + j]);
							rows[numrows + j] = newtup;
						}
						free_conversion_map(map);
					}
				}

				/* And add to counts */
				numrows += childrows;
				*totalrows += trows;
				*totaldeadrows += tdrows;
			}
		}

		/*
		 * Note: we cannot release the child-table locks, since we may have
		 * pointers to their TOAST tables in the sampled rows.
		 */
		table_close(childrel, NoLock);
		pgstat_progress_update_param(PROGRESS_ANALYZE_CHILD_TABLES_DONE,
									 i + 1);
	}

	return numrows;
}


/*
 *	update_attstats() -- update attribute statistics for one relation
 *
 *		Statistics are stored in several places: the pg_class row for the
 *		relation has stats about the whole relation, and there is a
 *		pg_statistic row for each (non-system) attribute that has ever
 *		been analyzed.  The pg_class values are updated by VACUUM, not here.
 *
 *		pg_statistic rows are just added or updated normally.  This means
 *		that pg_statistic will probably contain some deleted rows at the
 *		completion of a vacuum cycle, unless it happens to get vacuumed last.
 *
 *		To keep things simple, we punt for pg_statistic, and don't try
 *		to compute or store rows for pg_statistic itself in pg_statistic.
 *		This could possibly be made to work, but it's not worth the trouble.
 *		Note analyze_rel() has seen to it that we won't come here when
 *		vacuuming pg_statistic itself.
 *
 *		Note: there would be a race condition here if two backends could
 *		ANALYZE the same table concurrently.  Presently, we lock that out
 *		by taking a self-exclusive lock on the relation in analyze_rel().
 */
static void
update_attstats(Oid relid, bool inh, int natts, VacAttrStats **vacattrstats)
{
	Relation	sd;
	int			attno;
	CatalogIndexState indstate = NULL;

	if (natts <= 0)
		return;					/* nothing to do */

	sd = table_open(StatisticRelationId, RowExclusiveLock);

	for (attno = 0; attno < natts; attno++)
	{
		VacAttrStats *stats = vacattrstats[attno];
		HeapTuple	stup,
					oldtup;
		int			i,
					k,
					n;
		Datum		values[Natts_pg_statistic];
		bool		nulls[Natts_pg_statistic];
		bool		replaces[Natts_pg_statistic];

		/* Ignore attr if we weren't able to collect stats */
		if (!stats->stats_valid)
			continue;

		/*
		 * Construct a new pg_statistic tuple
		 */
		for (i = 0; i < Natts_pg_statistic; ++i)
		{
			nulls[i] = false;
			replaces[i] = true;
		}

		values[Anum_pg_statistic_starelid - 1] = ObjectIdGetDatum(relid);
		values[Anum_pg_statistic_staattnum - 1] = Int16GetDatum(stats->tupattnum);
		values[Anum_pg_statistic_stainherit - 1] = BoolGetDatum(inh);
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

		/* Is there already a pg_statistic tuple for this attribute? */
		oldtup = SearchSysCache3(STATRELATTINH,
								 ObjectIdGetDatum(relid),
								 Int16GetDatum(stats->tupattnum),
								 BoolGetDatum(inh));

		/* Open index information when we know we need it */
		if (indstate == NULL)
			indstate = CatalogOpenIndexes(sd);

		if (HeapTupleIsValid(oldtup))
		{
			/* Yes, replace it */
			stup = heap_modify_tuple(oldtup,
									 RelationGetDescr(sd),
									 values,
									 nulls,
									 replaces);
			ReleaseSysCache(oldtup);
			CatalogTupleUpdateWithInfo(sd, &stup->t_self, stup, indstate);
		}
		else
		{
			/* No, insert new tuple */
			stup = heap_form_tuple(RelationGetDescr(sd), values, nulls);
			CatalogTupleInsertWithInfo(sd, stup, indstate);
		}

		heap_freetuple(stup);
	}

	if (indstate != NULL)
		CatalogCloseIndexes(indstate);
	table_close(sd, RowExclusiveLock);
}

/*
 * Standard fetch function for use by compute_stats subroutines.
 *
 * This exists to provide some insulation between compute_stats routines
 * and the actual storage of the sample data.
 */
static Datum
std_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			attnum = stats->tupattnum;
	HeapTuple	tuple = stats->rows[rownum];
	TupleDesc	tupDesc = stats->tupDesc;

	return heap_getattr(tuple, attnum, tupDesc, isNull);
}

/*
 * Fetch function for analyzing index expressions.
 *
 * We have not bothered to construct index tuples, instead the data is
 * just in Datum arrays.
 */
static Datum
ind_fetch_func(VacAttrStatsP stats, int rownum, bool *isNull)
{
	int			i;

	/* exprvals and exprnulls are already offset for proper column */
	i = rownum * stats->rowstride;
	*isNull = stats->exprnulls[i];
	return stats->exprvals[i];
}


/*==========================================================================
 *
 * Code below this point represents the "standard" type-specific statistics
 * analysis algorithms.  This code can be replaced on a per-data-type basis
 * by setting a nonzero value in pg_type.typanalyze.
 *
 *==========================================================================
 */


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

#define swapInt(a,b)	do {int _tmp; _tmp=a; a=b; b=_tmp;} while(0)
#define swapDatum(a,b)	do {Datum _tmp; _tmp=a; a=b; b=_tmp;} while(0)

/*
 * Extra information used by the default analysis routines
 */
typedef struct
{
	int			count;			/* # of duplicates */
	int			first;			/* values[] index of first occurrence */
} ScalarMCVItem;

typedef struct
{
	SortSupport ssup;
	int		   *tupnoLink;
} CompareScalarsContext;


static void compute_trivial_stats(VacAttrStatsP stats,
								  AnalyzeAttrFetchFunc fetchfunc,
								  int samplerows,
								  double totalrows);
static void compute_distinct_stats(VacAttrStatsP stats,
								   AnalyzeAttrFetchFunc fetchfunc,
								   int samplerows,
								   double totalrows);
static void compute_scalar_stats(VacAttrStatsP stats,
								 AnalyzeAttrFetchFunc fetchfunc,
								 int samplerows,
								 double totalrows);
static int	compare_scalars(const void *a, const void *b, void *arg);
static int	compare_mcvs(const void *a, const void *b, void *arg);
static int	analyze_mcv_list(int *mcv_counts,
							 int num_mcv,
							 double stadistinct,
							 double stanullfrac,
							 int samplerows,
							 double totalrows);


/*
 * std_typanalyze -- the default type-specific typanalyze function
 */
bool
std_typanalyze(VacAttrStats *stats)
{
	Oid			ltopr;
	Oid			eqopr;
	StdAnalyzeData *mystats;

	/* If the attstattarget column is negative, use the default value */
	if (stats->attstattarget < 0)
		stats->attstattarget = default_statistics_target;

	/* Look for default "<" and "=" operators for column's type */
	get_sort_group_operators(stats->attrtypid,
							 false, false, false,
							 &ltopr, &eqopr, NULL,
							 NULL);

	/* Save the operator info for compute_stats routines */
	mystats = (StdAnalyzeData *) palloc(sizeof(StdAnalyzeData));
	mystats->eqopr = eqopr;
	mystats->eqfunc = OidIsValid(eqopr) ? get_opcode(eqopr) : InvalidOid;
	mystats->ltopr = ltopr;
	stats->extra_data = mystats;

	/*
	 * Determine which standard statistics algorithm to use
	 */
	if (OidIsValid(eqopr) && OidIsValid(ltopr))
	{
		/* Seems to be a scalar datatype */
		stats->compute_stats = compute_scalar_stats;
		/*--------------------
		 * The following choice of minrows is based on the paper
		 * "Random sampling for histogram construction: how much is enough?"
		 * by Surajit Chaudhuri, Rajeev Motwani and Vivek Narasayya, in
		 * Proceedings of ACM SIGMOD International Conference on Management
		 * of Data, 1998, Pages 436-447.  Their Corollary 1 to Theorem 5
		 * says that for table size n, histogram size k, maximum relative
		 * error in bin size f, and error probability gamma, the minimum
		 * random sample size is
		 *		r = 4 * k * ln(2*n/gamma) / f^2
		 * Taking f = 0.5, gamma = 0.01, n = 10^6 rows, we obtain
		 *		r = 305.82 * k
		 * Note that because of the log function, the dependence on n is
		 * quite weak; even at n = 10^12, a 300*k sample gives <= 0.66
		 * bin size error with probability 0.99.  So there's no real need to
		 * scale for n, which is a good thing because we don't necessarily
		 * know it at this point.
		 *--------------------
		 */
		stats->minrows = 300 * stats->attstattarget;
	}
	else if (OidIsValid(eqopr))
	{
		/* We can still recognize distinct values */
		stats->compute_stats = compute_distinct_stats;
		/* Might as well use the same minrows as above */
		stats->minrows = 300 * stats->attstattarget;
	}
	else
	{
		/* Can't do much but the trivial stuff */
		stats->compute_stats = compute_trivial_stats;
		/* Might as well use the same minrows as above */
		stats->minrows = 300 * stats->attstattarget;
	}

	return true;
}


/*
 *	compute_trivial_stats() -- compute very basic column statistics
 *
 *	We use this when we cannot find a hash "=" operator for the datatype.
 *
 *	We determine the fraction of non-null rows and the average datum width.
 */
static void
compute_trivial_stats(VacAttrStatsP stats,
					  AnalyzeAttrFetchFunc fetchfunc,
					  int samplerows,
					  double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);

	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;

		vacuum_delay_point(true);

		value = fetchfunc(stats, i, &isnull);

		/* Check for null/nonnull */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a variable-width field, add up widths for average width
		 * calculation.  Note that if the value is toasted, we use the toasted
		 * width.  We don't bother with this calculation if it's a fixed-width
		 * type.
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));
		}
		else if (is_varwidth)
		{
			/* must be cstring */
			total_width += strlen(DatumGetCString(value)) + 1;
		}
	}

	/* We can only compute average width if we found some non-null values. */
	if (nonnull_cnt > 0)
	{
		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}
	else if (null_cnt > 0)
	{
		/* We found only nulls; assume the column is entirely null */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}
}


/*
 *	compute_distinct_stats() -- compute column statistics including ndistinct
 *
 *	We use this when we can find only an "=" operator for the datatype.
 *
 *	We determine the fraction of non-null rows, the average width, the
 *	most common values, and the (estimated) number of distinct values.
 *
 *	The most common values are determined by brute force: we keep a list
 *	of previously seen values, ordered by number of times seen, as we scan
 *	the samples.  A newly seen value is inserted just after the last
 *	multiply-seen value, causing the bottommost (oldest) singly-seen value
 *	to drop off the list.  The accuracy of this method, and also its cost,
 *	depend mainly on the length of the list we are willing to keep.
 */
static void
compute_distinct_stats(VacAttrStatsP stats,
					   AnalyzeAttrFetchFunc fetchfunc,
					   int samplerows,
					   double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	int			toowide_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);
	FmgrInfo	f_cmpeq;
	typedef struct
	{
		Datum		value;
		int			count;
	} TrackItem;
	TrackItem  *track;
	int			track_cnt,
				track_max;
	int			num_mcv = stats->attstattarget;
	StdAnalyzeData *mystats = (StdAnalyzeData *) stats->extra_data;

	/*
	 * We track up to 2*n values for an n-element MCV list; but at least 10
	 */
	track_max = 2 * num_mcv;
	if (track_max < 10)
		track_max = 10;
	track = (TrackItem *) palloc(track_max * sizeof(TrackItem));
	track_cnt = 0;

	fmgr_info(mystats->eqfunc, &f_cmpeq);

	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;
		bool		match;
		int			firstcount1,
					j;

		vacuum_delay_point(true);

		value = fetchfunc(stats, i, &isnull);

		/* Check for null/nonnull */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a variable-width field, add up widths for average width
		 * calculation.  Note that if the value is toasted, we use the toasted
		 * width.  We don't bother with this calculation if it's a fixed-width
		 * type.
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));

			/*
			 * If the value is toasted, we want to detoast it just once to
			 * avoid repeated detoastings and resultant excess memory usage
			 * during the comparisons.  Also, check to see if the value is
			 * excessively wide, and if so don't detoast at all --- just
			 * ignore the value.
			 */
			if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
			{
				toowide_cnt++;
				continue;
			}
			value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}
		else if (is_varwidth)
		{
			/* must be cstring */
			total_width += strlen(DatumGetCString(value)) + 1;
		}

		/*
		 * See if the value matches anything we're already tracking.
		 */
		match = false;
		firstcount1 = track_cnt;
		for (j = 0; j < track_cnt; j++)
		{
			if (DatumGetBool(FunctionCall2Coll(&f_cmpeq,
											   stats->attrcollid,
											   value, track[j].value)))
			{
				match = true;
				break;
			}
			if (j < firstcount1 && track[j].count == 1)
				firstcount1 = j;
		}

		if (match)
		{
			/* Found a match */
			track[j].count++;
			/* This value may now need to "bubble up" in the track list */
			while (j > 0 && track[j].count > track[j - 1].count)
			{
				swapDatum(track[j].value, track[j - 1].value);
				swapInt(track[j].count, track[j - 1].count);
				j--;
			}
		}
		else
		{
			/* No match.  Insert at head of count-1 list */
			if (track_cnt < track_max)
				track_cnt++;
			for (j = track_cnt - 1; j > firstcount1; j--)
			{
				track[j].value = track[j - 1].value;
				track[j].count = track[j - 1].count;
			}
			if (firstcount1 < track_cnt)
			{
				track[firstcount1].value = value;
				track[firstcount1].count = 1;
			}
		}
	}

	/* We can only compute real stats if we found some non-null values. */
	if (nonnull_cnt > 0)
	{
		int			nmultiple,
					summultiple;

		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		/* Count the number of values we found multiple times */
		summultiple = 0;
		for (nmultiple = 0; nmultiple < track_cnt; nmultiple++)
		{
			if (track[nmultiple].count == 1)
				break;
			summultiple += track[nmultiple].count;
		}

		if (nmultiple == 0)
		{
			/*
			 * If we found no repeated non-null values, assume it's a unique
			 * column; but be sure to discount for any nulls we found.
			 */
			stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
		}
		else if (track_cnt < track_max && toowide_cnt == 0 &&
				 nmultiple == track_cnt)
		{
			/*
			 * Our track list includes every value in the sample, and every
			 * value appeared more than once.  Assume the column has just
			 * these values.  (This case is meant to address columns with
			 * small, fixed sets of possible values, such as boolean or enum
			 * columns.  If there are any values that appear just once in the
			 * sample, including too-wide values, we should assume that that's
			 * not what we're dealing with.)
			 */
			stats->stadistinct = track_cnt;
		}
		else
		{
			/*----------
			 * Estimate the number of distinct values using the estimator
			 * proposed by Haas and Stokes in IBM Research Report RJ 10025:
			 *		n*d / (n - f1 + f1*n/N)
			 * where f1 is the number of distinct values that occurred
			 * exactly once in our sample of n rows (from a total of N),
			 * and d is the total number of distinct values in the sample.
			 * This is their Duj1 estimator; the other estimators they
			 * recommend are considerably more complex, and are numerically
			 * very unstable when n is much smaller than N.
			 *
			 * In this calculation, we consider only non-nulls.  We used to
			 * include rows with null values in the n and N counts, but that
			 * leads to inaccurate answers in columns with many nulls, and
			 * it's intuitively bogus anyway considering the desired result is
			 * the number of distinct non-null values.
			 *
			 * We assume (not very reliably!) that all the multiply-occurring
			 * values are reflected in the final track[] list, and the other
			 * nonnull values all appeared but once.  (XXX this usually
			 * results in a drastic overestimate of ndistinct.  Can we do
			 * any better?)
			 *----------
			 */
			int			f1 = nonnull_cnt - summultiple;
			int			d = f1 + nmultiple;
			double		n = samplerows - null_cnt;
			double		N = totalrows * (1.0 - stats->stanullfrac);
			double		stadistinct;

			/* N == 0 shouldn't happen, but just in case ... */
			if (N > 0)
				stadistinct = (n * d) / ((n - f1) + f1 * n / N);
			else
				stadistinct = 0;

			/* Clamp to sane range in case of roundoff error */
			if (stadistinct < d)
				stadistinct = d;
			if (stadistinct > N)
				stadistinct = N;
			/* And round to integer */
			stats->stadistinct = floor(stadistinct + 0.5);
		}

		/*
		 * If we estimated the number of distinct values at more than 10% of
		 * the total row count (a very arbitrary limit), then assume that
		 * stadistinct should scale with the row count rather than be a fixed
		 * value.
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = -(stats->stadistinct / totalrows);

		/*
		 * Decide how many values are worth storing as most-common values. If
		 * we are able to generate a complete MCV list (all the values in the
		 * sample will fit, and we think these are all the ones in the table),
		 * then do so.  Otherwise, store only those values that are
		 * significantly more common than the values not in the list.
		 *
		 * Note: the first of these cases is meant to address columns with
		 * small, fixed sets of possible values, such as boolean or enum
		 * columns.  If we can *completely* represent the column population by
		 * an MCV list that will fit into the stats target, then we should do
		 * so and thus provide the planner with complete information.  But if
		 * the MCV list is not complete, it's generally worth being more
		 * selective, and not just filling it all the way up to the stats
		 * target.
		 */
		if (track_cnt < track_max && toowide_cnt == 0 &&
			stats->stadistinct > 0 &&
			track_cnt <= num_mcv)
		{
			/* Track list includes all values seen, and all will fit */
			num_mcv = track_cnt;
		}
		else
		{
			int		   *mcv_counts;

			/* Incomplete list; decide how many values are worth keeping */
			if (num_mcv > track_cnt)
				num_mcv = track_cnt;

			if (num_mcv > 0)
			{
				mcv_counts = (int *) palloc(num_mcv * sizeof(int));
				for (i = 0; i < num_mcv; i++)
					mcv_counts[i] = track[i].count;

				num_mcv = analyze_mcv_list(mcv_counts, num_mcv,
										   stats->stadistinct,
										   stats->stanullfrac,
										   samplerows, totalrows);
			}
		}

		/* Generate MCV slot entry */
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum	   *mcv_values;
			float4	   *mcv_freqs;

			/* Must copy the target values into anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			mcv_values = (Datum *) palloc(num_mcv * sizeof(Datum));
			mcv_freqs = (float4 *) palloc(num_mcv * sizeof(float4));
			for (i = 0; i < num_mcv; i++)
			{
				mcv_values[i] = datumCopy(track[i].value,
										  stats->attrtype->typbyval,
										  stats->attrtype->typlen);
				mcv_freqs[i] = (double) track[i].count / (double) samplerows;
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[0] = STATISTIC_KIND_MCV;
			stats->staop[0] = mystats->eqopr;
			stats->stacoll[0] = stats->attrcollid;
			stats->stanumbers[0] = mcv_freqs;
			stats->numnumbers[0] = num_mcv;
			stats->stavalues[0] = mcv_values;
			stats->numvalues[0] = num_mcv;

			/*
			 * Accept the defaults for stats->statypid and others. They have
			 * been set before we were called (see vacuum.h)
			 */
		}
	}
	else if (null_cnt > 0)
	{
		/* We found only nulls; assume the column is entirely null */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}

	/* We don't need to bother cleaning up any of our temporary palloc's */
}


/*
 *	compute_scalar_stats() -- compute column statistics
 *
 *	We use this when we can find "=" and "<" operators for the datatype.
 *
 *	We determine the fraction of non-null rows, the average width, the
 *	most common values, the (estimated) number of distinct values, the
 *	distribution histogram, and the correlation of physical to logical order.
 *
 *	The desired stats can be determined fairly easily after sorting the
 *	data values into order.
 */
static void
compute_scalar_stats(VacAttrStatsP stats,
					 AnalyzeAttrFetchFunc fetchfunc,
					 int samplerows,
					 double totalrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	int			toowide_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attrtype->typbyval &&
							  stats->attrtype->typlen == -1);
	bool		is_varwidth = (!stats->attrtype->typbyval &&
							   stats->attrtype->typlen < 0);
	double		corr_xysum;
	SortSupportData ssup;
	ScalarItem *values;
	int			values_cnt = 0;
	int		   *tupnoLink;
	ScalarMCVItem *track;
	int			track_cnt = 0;
	int			num_mcv = stats->attstattarget;
	int			num_bins = stats->attstattarget;
	StdAnalyzeData *mystats = (StdAnalyzeData *) stats->extra_data;

	values = (ScalarItem *) palloc(samplerows * sizeof(ScalarItem));
	tupnoLink = (int *) palloc(samplerows * sizeof(int));
	track = (ScalarMCVItem *) palloc(num_mcv * sizeof(ScalarMCVItem));

	memset(&ssup, 0, sizeof(ssup));
	ssup.ssup_cxt = CurrentMemoryContext;
	ssup.ssup_collation = stats->attrcollid;
	ssup.ssup_nulls_first = false;

	/*
	 * For now, don't perform abbreviated key conversion, because full values
	 * are required for MCV slot generation.  Supporting that optimization
	 * would necessitate teaching compare_scalars() to call a tie-breaker.
	 */
	ssup.abbreviate = false;

	PrepareSortSupportFromOrderingOp(mystats->ltopr, &ssup);

	/* Initial scan to find sortable values */
	for (i = 0; i < samplerows; i++)
	{
		Datum		value;
		bool		isnull;

		vacuum_delay_point(true);

		value = fetchfunc(stats, i, &isnull);

		/* Check for null/nonnull */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a variable-width field, add up widths for average width
		 * calculation.  Note that if the value is toasted, we use the toasted
		 * width.  We don't bother with this calculation if it's a fixed-width
		 * type.
		 */
		if (is_varlena)
		{
			total_width += VARSIZE_ANY(DatumGetPointer(value));

			/*
			 * If the value is toasted, we want to detoast it just once to
			 * avoid repeated detoastings and resultant excess memory usage
			 * during the comparisons.  Also, check to see if the value is
			 * excessively wide, and if so don't detoast at all --- just
			 * ignore the value.
			 */
			if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
			{
				toowide_cnt++;
				continue;
			}
			value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}
		else if (is_varwidth)
		{
			/* must be cstring */
			total_width += strlen(DatumGetCString(value)) + 1;
		}

		/* Add it to the list to be sorted */
		values[values_cnt].value = value;
		values[values_cnt].tupno = values_cnt;
		tupnoLink[values_cnt] = values_cnt;
		values_cnt++;
	}

	/* We can only compute real stats if we found some sortable values. */
	if (values_cnt > 0)
	{
		int			ndistinct,	/* # distinct values in sample */
					nmultiple,	/* # that appear multiple times */
					num_hist,
					dups_cnt;
		int			slot_idx = 0;
		CompareScalarsContext cxt;

		/* Sort the collected values */
		cxt.ssup = &ssup;
		cxt.tupnoLink = tupnoLink;
		qsort_interruptible(values, values_cnt, sizeof(ScalarItem),
							compare_scalars, &cxt);

		/*
		 * Now scan the values in order, find the most common ones, and also
		 * accumulate ordering-correlation statistics.
		 *
		 * To determine which are most common, we first have to count the
		 * number of duplicates of each value.  The duplicates are adjacent in
		 * the sorted list, so a brute-force approach is to compare successive
		 * datum values until we find two that are not equal. However, that
		 * requires N-1 invocations of the datum comparison routine, which are
		 * completely redundant with work that was done during the sort.  (The
		 * sort algorithm must at some point have compared each pair of items
		 * that are adjacent in the sorted order; otherwise it could not know
		 * that it's ordered the pair correctly.) We exploit this by having
		 * compare_scalars remember the highest tupno index that each
		 * ScalarItem has been found equal to.  At the end of the sort, a
		 * ScalarItem's tupnoLink will still point to itself if and only if it
		 * is the last item of its group of duplicates (since the group will
		 * be ordered by tupno).
		 */
		corr_xysum = 0;
		ndistinct = 0;
		nmultiple = 0;
		dups_cnt = 0;
		for (i = 0; i < values_cnt; i++)
		{
			int			tupno = values[i].tupno;

			corr_xysum += ((double) i) * ((double) tupno);
			dups_cnt++;
			if (tupnoLink[tupno] == tupno)
			{
				/* Reached end of duplicates of this value */
				ndistinct++;
				if (dups_cnt > 1)
				{
					nmultiple++;
					if (track_cnt < num_mcv ||
						dups_cnt > track[track_cnt - 1].count)
					{
						/*
						 * Found a new item for the mcv list; find its
						 * position, bubbling down old items if needed. Loop
						 * invariant is that j points at an empty/ replaceable
						 * slot.
						 */
						int			j;

						if (track_cnt < num_mcv)
							track_cnt++;
						for (j = track_cnt - 1; j > 0; j--)
						{
							if (dups_cnt <= track[j - 1].count)
								break;
							track[j].count = track[j - 1].count;
							track[j].first = track[j - 1].first;
						}
						track[j].count = dups_cnt;
						track[j].first = i + 1 - dups_cnt;
					}
				}
				dups_cnt = 0;
			}
		}

		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		if (nmultiple == 0)
		{
			/*
			 * If we found no repeated non-null values, assume it's a unique
			 * column; but be sure to discount for any nulls we found.
			 */
			stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
		}
		else if (toowide_cnt == 0 && nmultiple == ndistinct)
		{
			/*
			 * Every value in the sample appeared more than once.  Assume the
			 * column has just these values.  (This case is meant to address
			 * columns with small, fixed sets of possible values, such as
			 * boolean or enum columns.  If there are any values that appear
			 * just once in the sample, including too-wide values, we should
			 * assume that that's not what we're dealing with.)
			 */
			stats->stadistinct = ndistinct;
		}
		else
		{
			/*----------
			 * Estimate the number of distinct values using the estimator
			 * proposed by Haas and Stokes in IBM Research Report RJ 10025:
			 *		n*d / (n - f1 + f1*n/N)
			 * where f1 is the number of distinct values that occurred
			 * exactly once in our sample of n rows (from a total of N),
			 * and d is the total number of distinct values in the sample.
			 * This is their Duj1 estimator; the other estimators they
			 * recommend are considerably more complex, and are numerically
			 * very unstable when n is much smaller than N.
			 *
			 * In this calculation, we consider only non-nulls.  We used to
			 * include rows with null values in the n and N counts, but that
			 * leads to inaccurate answers in columns with many nulls, and
			 * it's intuitively bogus anyway considering the desired result is
			 * the number of distinct non-null values.
			 *
			 * Overwidth values are assumed to have been distinct.
			 *----------
			 */
			int			f1 = ndistinct - nmultiple + toowide_cnt;
			int			d = f1 + nmultiple;
			double		n = samplerows - null_cnt;
			double		N = totalrows * (1.0 - stats->stanullfrac);
			double		stadistinct;

			/* N == 0 shouldn't happen, but just in case ... */
			if (N > 0)
				stadistinct = (n * d) / ((n - f1) + f1 * n / N);
			else
				stadistinct = 0;

			/* Clamp to sane range in case of roundoff error */
			if (stadistinct < d)
				stadistinct = d;
			if (stadistinct > N)
				stadistinct = N;
			/* And round to integer */
			stats->stadistinct = floor(stadistinct + 0.5);
		}

		/*
		 * If we estimated the number of distinct values at more than 10% of
		 * the total row count (a very arbitrary limit), then assume that
		 * stadistinct should scale with the row count rather than be a fixed
		 * value.
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = -(stats->stadistinct / totalrows);

		/*
		 * Decide how many values are worth storing as most-common values. If
		 * we are able to generate a complete MCV list (all the values in the
		 * sample will fit, and we think these are all the ones in the table),
		 * then do so.  Otherwise, store only those values that are
		 * significantly more common than the values not in the list.
		 *
		 * Note: the first of these cases is meant to address columns with
		 * small, fixed sets of possible values, such as boolean or enum
		 * columns.  If we can *completely* represent the column population by
		 * an MCV list that will fit into the stats target, then we should do
		 * so and thus provide the planner with complete information.  But if
		 * the MCV list is not complete, it's generally worth being more
		 * selective, and not just filling it all the way up to the stats
		 * target.
		 */
		if (track_cnt == ndistinct && toowide_cnt == 0 &&
			stats->stadistinct > 0 &&
			track_cnt <= num_mcv)
		{
			/* Track list includes all values seen, and all will fit */
			num_mcv = track_cnt;
		}
		else
		{
			int		   *mcv_counts;

			/* Incomplete list; decide how many values are worth keeping */
			if (num_mcv > track_cnt)
				num_mcv = track_cnt;

			if (num_mcv > 0)
			{
				mcv_counts = (int *) palloc(num_mcv * sizeof(int));
				for (i = 0; i < num_mcv; i++)
					mcv_counts[i] = track[i].count;

				num_mcv = analyze_mcv_list(mcv_counts, num_mcv,
										   stats->stadistinct,
										   stats->stanullfrac,
										   samplerows, totalrows);
			}
		}

		/* Generate MCV slot entry */
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum	   *mcv_values;
			float4	   *mcv_freqs;

			/* Must copy the target values into anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			mcv_values = (Datum *) palloc(num_mcv * sizeof(Datum));
			mcv_freqs = (float4 *) palloc(num_mcv * sizeof(float4));
			for (i = 0; i < num_mcv; i++)
			{
				mcv_values[i] = datumCopy(values[track[i].first].value,
										  stats->attrtype->typbyval,
										  stats->attrtype->typlen);
				mcv_freqs[i] = (double) track[i].count / (double) samplerows;
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_MCV;
			stats->staop[slot_idx] = mystats->eqopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stanumbers[slot_idx] = mcv_freqs;
			stats->numnumbers[slot_idx] = num_mcv;
			stats->stavalues[slot_idx] = mcv_values;
			stats->numvalues[slot_idx] = num_mcv;

			/*
			 * Accept the defaults for stats->statypid and others. They have
			 * been set before we were called (see vacuum.h)
			 */
			slot_idx++;
		}

		/*
		 * Generate a histogram slot entry if there are at least two distinct
		 * values not accounted for in the MCV list.  (This ensures the
		 * histogram won't collapse to empty or a singleton.)
		 */
		num_hist = ndistinct - num_mcv;
		if (num_hist > num_bins)
			num_hist = num_bins + 1;
		if (num_hist >= 2)
		{
			MemoryContext old_context;
			Datum	   *hist_values;
			int			nvals;
			int			pos,
						posfrac,
						delta,
						deltafrac;

			/* Sort the MCV items into position order to speed next loop */
			qsort_interruptible(track, num_mcv, sizeof(ScalarMCVItem),
								compare_mcvs, NULL);

			/*
			 * Collapse out the MCV items from the values[] array.
			 *
			 * Note we destroy the values[] array here... but we don't need it
			 * for anything more.  We do, however, still need values_cnt.
			 * nvals will be the number of remaining entries in values[].
			 */
			if (num_mcv > 0)
			{
				int			src,
							dest;
				int			j;

				src = dest = 0;
				j = 0;			/* index of next interesting MCV item */
				while (src < values_cnt)
				{
					int			ncopy;

					if (j < num_mcv)
					{
						int			first = track[j].first;

						if (src >= first)
						{
							/* advance past this MCV item */
							src = first + track[j].count;
							j++;
							continue;
						}
						ncopy = first - src;
					}
					else
						ncopy = values_cnt - src;
					memmove(&values[dest], &values[src],
							ncopy * sizeof(ScalarItem));
					src += ncopy;
					dest += ncopy;
				}
				nvals = dest;
			}
			else
				nvals = values_cnt;
			Assert(nvals >= num_hist);

			/* Must copy the target values into anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * The object of this loop is to copy the first and last values[]
			 * entries along with evenly-spaced values in between.  So the
			 * i'th value is values[(i * (nvals - 1)) / (num_hist - 1)].  But
			 * computing that subscript directly risks integer overflow when
			 * the stats target is more than a couple thousand.  Instead we
			 * add (nvals - 1) / (num_hist - 1) to pos at each step, tracking
			 * the integral and fractional parts of the sum separately.
			 */
			delta = (nvals - 1) / (num_hist - 1);
			deltafrac = (nvals - 1) % (num_hist - 1);
			pos = posfrac = 0;

			for (i = 0; i < num_hist; i++)
			{
				hist_values[i] = datumCopy(values[pos].value,
										   stats->attrtype->typbyval,
										   stats->attrtype->typlen);
				pos += delta;
				posfrac += deltafrac;
				if (posfrac >= (num_hist - 1))
				{
					/* fractional part exceeds 1, carry to integer part */
					pos++;
					posfrac -= (num_hist - 1);
				}
			}

			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_HISTOGRAM;
			stats->staop[slot_idx] = mystats->ltopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stavalues[slot_idx] = hist_values;
			stats->numvalues[slot_idx] = num_hist;

			/*
			 * Accept the defaults for stats->statypid and others. They have
			 * been set before we were called (see vacuum.h)
			 */
			slot_idx++;
		}

		/* Generate a correlation entry if there are multiple values */
		if (values_cnt > 1)
		{
			MemoryContext old_context;
			float4	   *corrs;
			double		corr_xsum,
						corr_x2sum;

			/* Must copy the target values into anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);
			corrs = (float4 *) palloc(sizeof(float4));
			MemoryContextSwitchTo(old_context);

			/*----------
			 * Since we know the x and y value sets are both
			 *		0, 1, ..., values_cnt-1
			 * we have sum(x) = sum(y) =
			 *		(values_cnt-1)*values_cnt / 2
			 * and sum(x^2) = sum(y^2) =
			 *		(values_cnt-1)*values_cnt*(2*values_cnt-1) / 6.
			 *----------
			 */
			corr_xsum = ((double) (values_cnt - 1)) *
				((double) values_cnt) / 2.0;
			corr_x2sum = ((double) (values_cnt - 1)) *
				((double) values_cnt) * (double) (2 * values_cnt - 1) / 6.0;

			/* And the correlation coefficient reduces to */
			corrs[0] = (values_cnt * corr_xysum - corr_xsum * corr_xsum) /
				(values_cnt * corr_x2sum - corr_xsum * corr_xsum);

			stats->stakind[slot_idx] = STATISTIC_KIND_CORRELATION;
			stats->staop[slot_idx] = mystats->ltopr;
			stats->stacoll[slot_idx] = stats->attrcollid;
			stats->stanumbers[slot_idx] = corrs;
			stats->numnumbers[slot_idx] = 1;
			slot_idx++;
		}
	}
	else if (nonnull_cnt > 0)
	{
		/* We found some non-null values, but they were all too wide */
		Assert(nonnull_cnt == toowide_cnt);
		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		if (is_varwidth)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;
		/* Assume all too-wide values are distinct, so it's a unique column */
		stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);
	}
	else if (null_cnt > 0)
	{
		/* We found only nulls; assume the column is entirely null */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		if (is_varwidth)
			stats->stawidth = 0;	/* "unknown" */
		else
			stats->stawidth = stats->attrtype->typlen;
		stats->stadistinct = 0.0;	/* "unknown" */
	}

	/* We don't need to bother cleaning up any of our temporary palloc's */
}

/*
 * Comparator for sorting ScalarItems
 *
 * Aside from sorting the items, we update the tupnoLink[] array
 * whenever two ScalarItems are found to contain equal datums.  The array
 * is indexed by tupno; for each ScalarItem, it contains the highest
 * tupno that that item's datum has been found to be equal to.  This allows
 * us to avoid additional comparisons in compute_scalar_stats().
 */
static int
compare_scalars(const void *a, const void *b, void *arg)
{
	Datum		da = ((const ScalarItem *) a)->value;
	int			ta = ((const ScalarItem *) a)->tupno;
	Datum		db = ((const ScalarItem *) b)->value;
	int			tb = ((const ScalarItem *) b)->tupno;
	CompareScalarsContext *cxt = (CompareScalarsContext *) arg;
	int			compare;

	compare = ApplySortComparator(da, false, db, false, cxt->ssup);
	if (compare != 0)
		return compare;

	/*
	 * The two datums are equal, so update cxt->tupnoLink[].
	 */
	if (cxt->tupnoLink[ta] < tb)
		cxt->tupnoLink[ta] = tb;
	if (cxt->tupnoLink[tb] < ta)
		cxt->tupnoLink[tb] = ta;

	/*
	 * For equal datums, sort by tupno
	 */
	return ta - tb;
}

/*
 * Comparator for sorting ScalarMCVItems by position
 */
static int
compare_mcvs(const void *a, const void *b, void *arg)
{
	int			da = ((const ScalarMCVItem *) a)->first;
	int			db = ((const ScalarMCVItem *) b)->first;

	return da - db;
}

/*
 * Analyze the list of common values in the sample and decide how many are
 * worth storing in the table's MCV list.
 *
 * mcv_counts is assumed to be a list of the counts of the most common values
 * seen in the sample, starting with the most common.  The return value is the
 * number that are significantly more common than the values not in the list,
 * and which are therefore deemed worth storing in the table's MCV list.
 */
static int
analyze_mcv_list(int *mcv_counts,
				 int num_mcv,
				 double stadistinct,
				 double stanullfrac,
				 int samplerows,
				 double totalrows)
{
	double		ndistinct_table;
	double		sumcount;
	int			i;

	/*
	 * If the entire table was sampled, keep the whole list.  This also
	 * protects us against division by zero in the code below.
	 */
	if (samplerows == totalrows || totalrows <= 1.0)
		return num_mcv;

	/* Re-extract the estimated number of distinct nonnull values in table */
	ndistinct_table = stadistinct;
	if (ndistinct_table < 0)
		ndistinct_table = -ndistinct_table * totalrows;

	/*
	 * Exclude the least common values from the MCV list, if they are not
	 * significantly more common than the estimated selectivity they would
	 * have if they weren't in the list.  All non-MCV values are assumed to be
	 * equally common, after taking into account the frequencies of all the
	 * values in the MCV list and the number of nulls (c.f. eqsel()).
	 *
	 * Here sumcount tracks the total count of all but the last (least common)
	 * value in the MCV list, allowing us to determine the effect of excluding
	 * that value from the list.
	 *
	 * Note that we deliberately do this by removing values from the full
	 * list, rather than starting with an empty list and adding values,
	 * because the latter approach can fail to add any values if all the most
	 * common values have around the same frequency and make up the majority
	 * of the table, so that the overall average frequency of all values is
	 * roughly the same as that of the common values.  This would lead to any
	 * uncommon values being significantly overestimated.
	 */
	sumcount = 0.0;
	for (i = 0; i < num_mcv - 1; i++)
		sumcount += mcv_counts[i];

	while (num_mcv > 0)
	{
		double		selec,
					otherdistinct,
					N,
					n,
					K,
					variance,
					stddev;

		/*
		 * Estimated selectivity the least common value would have if it
		 * wasn't in the MCV list (c.f. eqsel()).
		 */
		selec = 1.0 - sumcount / samplerows - stanullfrac;
		if (selec < 0.0)
			selec = 0.0;
		if (selec > 1.0)
			selec = 1.0;
		otherdistinct = ndistinct_table - (num_mcv - 1);
		if (otherdistinct > 1)
			selec /= otherdistinct;

		/*
		 * If the value is kept in the MCV list, its population frequency is
		 * assumed to equal its sample frequency.  We use the lower end of a
		 * textbook continuity-corrected Wald-type confidence interval to
		 * determine if that is significantly more common than the non-MCV
		 * frequency --- specifically we assume the population frequency is
		 * highly likely to be within around 2 standard errors of the sample
		 * frequency, which equates to an interval of 2 standard deviations
		 * either side of the sample count, plus an additional 0.5 for the
		 * continuity correction.  Since we are sampling without replacement,
		 * this is a hypergeometric distribution.
		 *
		 * XXX: Empirically, this approach seems to work quite well, but it
		 * may be worth considering more advanced techniques for estimating
		 * the confidence interval of the hypergeometric distribution.
		 */
		N = totalrows;
		n = samplerows;
		K = N * mcv_counts[num_mcv - 1] / n;
		variance = n * K * (N - K) * (N - n) / (N * N * (N - 1));
		stddev = sqrt(variance);

		if (mcv_counts[num_mcv - 1] > selec * samplerows + 2 * stddev + 0.5)
		{
			/*
			 * The value is significantly more common than the non-MCV
			 * selectivity would suggest.  Keep it, and all the other more
			 * common values in the list.
			 */
			break;
		}
		else
		{
			/* Discard this value and consider the next least common value */
			num_mcv--;
			if (num_mcv == 0)
				break;
			sumcount -= mcv_counts[num_mcv - 1];
		}
	}
	return num_mcv;
}
