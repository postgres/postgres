/*-------------------------------------------------------------------------
 *
 * postgres_fdw.c
 *		  Foreign-data wrapper for remote PostgreSQL servers
 *
 * Portions Copyright (c) 2012-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/postgres_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "postgres_fdw.h"

#include "access/htup_details.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


PG_MODULE_MAGIC;

/* Default CPU cost to start up a foreign query. */
#define DEFAULT_FDW_STARTUP_COST	100.0

/* Default CPU cost to process 1 row (above and beyond cpu_tuple_cost). */
#define DEFAULT_FDW_TUPLE_COST		0.01

/*
 * FDW-specific planner information kept in RelOptInfo.fdw_private for a
 * foreign table.  This information is collected by postgresGetForeignRelSize.
 */
typedef struct PgFdwRelationInfo
{
	/* XXX underdocumented, but a lot of this shouldn't be here anyway */
	StringInfoData sql;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *remote_conds;
	List	   *param_conds;
	List	   *local_conds;
	List	   *param_numbers;

	/* Cached catalog information. */
	ForeignTable *table;
	ForeignServer *server;
} PgFdwRelationInfo;

/*
 * Indexes of FDW-private information stored in fdw_private list.
 *
 * We store various information in ForeignScan.fdw_private to pass it from
 * planner to executor.  Specifically there is:
 *
 * 1) SELECT statement text to be sent to the remote server
 * 2) IDs of PARAM_EXEC Params used in the SELECT statement
 *
 * These items are indexed with the enum FdwPrivateIndex, so an item can be
 * fetched with list_nth().  For example, to get the SELECT statement:
 *		sql = strVal(list_nth(fdw_private, FdwPrivateSelectSql));
 */
enum FdwPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwPrivateSelectSql,

	/* Integer list of param IDs of PARAM_EXEC Params used in SQL stmt */
	FdwPrivateExternParamIds,

	/* # of elements stored in the list fdw_private */
	FdwPrivateNum
};

/*
 * Execution state of a foreign scan using postgres_fdw.
 */
typedef struct PgFdwExecutionState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	List	   *fdw_private;	/* FDW-private information from planner */

	/* for remote query execution */
	PGconn	   *conn;			/* connection for the scan */
	unsigned int cursor_number; /* quasi-unique ID for my cursor */
	bool		cursor_exists;	/* have we created the cursor? */
	bool		extparams_done; /* have we converted PARAM_EXTERN params? */
	int			numParams;		/* number of parameters passed to query */
	Oid		   *param_types;	/* array of types of query parameters */
	const char **param_values;	/* array of values of query parameters */

	/* for storing result tuples */
	HeapTuple  *tuples;			/* array of currently-retrieved tuples */
	int			num_tuples;		/* # of tuples in array */
	int			next_tuple;		/* index of next one to return */

	/* batch-level state, for optimizing rewinds and avoiding useless fetch */
	int			fetch_ct_2;		/* Min(# of fetches done, 2) */
	bool		eof_reached;	/* true if last fetch reached EOF */

	/* working memory contexts */
	MemoryContext batch_cxt;	/* context holding current batch of tuples */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} PgFdwExecutionState;

/*
 * Workspace for analyzing a foreign table.
 */
typedef struct PgFdwAnalyzeState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* collected sample rows */
	HeapTuple  *rows;			/* array of size targrows */
	int			targrows;		/* target # of sample rows */
	int			numrows;		/* # of sample rows collected */

	/* for random sampling */
	double		samplerows;		/* # of rows fetched */
	double		rowstoskip;		/* # of rows to skip before next sample */
	double		rstate;			/* random state */

	/* working memory contexts */
	MemoryContext anl_cxt;		/* context for per-analyze lifespan data */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} PgFdwAnalyzeState;

/*
 * Identify the attribute where data conversion fails.
 */
typedef struct ConversionLocation
{
	Relation	rel;			/* foreign table's relcache entry */
	AttrNumber	cur_attno;		/* attribute number being processed, or 0 */
} ConversionLocation;

/*
 * SQL functions
 */
extern Datum postgres_fdw_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(postgres_fdw_handler);

/*
 * FDW callback routines
 */
static void postgresGetForeignRelSize(PlannerInfo *root,
						  RelOptInfo *baserel,
						  Oid foreigntableid);
static void postgresGetForeignPaths(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid);
static ForeignScan *postgresGetForeignPlan(PlannerInfo *root,
					   RelOptInfo *baserel,
					   Oid foreigntableid,
					   ForeignPath *best_path,
					   List *tlist,
					   List *scan_clauses);
static void postgresExplainForeignScan(ForeignScanState *node,
						   ExplainState *es);
static void postgresBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *postgresIterateForeignScan(ForeignScanState *node);
static void postgresReScanForeignScan(ForeignScanState *node);
static void postgresEndForeignScan(ForeignScanState *node);
static bool postgresAnalyzeForeignTable(Relation relation,
							AcquireSampleRowsFunc *func,
							BlockNumber *totalpages);

/*
 * Helper functions
 */
static void get_remote_estimate(const char *sql,
					PGconn *conn,
					double *rows,
					int *width,
					Cost *startup_cost,
					Cost *total_cost);
static void create_cursor(ForeignScanState *node);
static void fetch_more_data(ForeignScanState *node);
static void close_cursor(PGconn *conn, unsigned int cursor_number);
static int postgresAcquireSampleRowsFunc(Relation relation, int elevel,
							  HeapTuple *rows, int targrows,
							  double *totalrows,
							  double *totaldeadrows);
static void analyze_row_processor(PGresult *res, int row,
					  PgFdwAnalyzeState *astate);
static HeapTuple make_tuple_from_result_row(PGresult *res,
						   int row,
						   Relation rel,
						   AttInMetadata *attinmeta,
						   MemoryContext temp_context);
static void conversion_error_callback(void *arg);


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
postgres_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* Required handler functions. */
	routine->GetForeignRelSize = postgresGetForeignRelSize;
	routine->GetForeignPaths = postgresGetForeignPaths;
	routine->GetForeignPlan = postgresGetForeignPlan;
	routine->ExplainForeignScan = postgresExplainForeignScan;
	routine->BeginForeignScan = postgresBeginForeignScan;
	routine->IterateForeignScan = postgresIterateForeignScan;
	routine->ReScanForeignScan = postgresReScanForeignScan;
	routine->EndForeignScan = postgresEndForeignScan;

	/* Optional handler functions. */
	routine->AnalyzeForeignTable = postgresAnalyzeForeignTable;

	PG_RETURN_POINTER(routine);
}

/*
 * postgresGetForeignRelSize
 *		Estimate # of rows and width of the result of the scan
 *
 * Here we estimate number of rows returned by the scan in two steps.  In the
 * first step, we execute remote EXPLAIN command to obtain the number of rows
 * returned from remote side.  In the second step, we calculate the selectivity
 * of the filtering done on local side, and modify first estimate.
 *
 * We have to get some catalog objects and generate remote query string here,
 * so we store such expensive information in FDW private area of RelOptInfo and
 * pass them to subsequent functions for reuse.
 */
static void
postgresGetForeignRelSize(PlannerInfo *root,
						  RelOptInfo *baserel,
						  Oid foreigntableid)
{
	bool		use_remote_explain = false;
	ListCell   *lc;
	PgFdwRelationInfo *fpinfo;
	StringInfo	sql;
	ForeignTable *table;
	ForeignServer *server;
	Selectivity sel;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	Cost		run_cost;
	QualCost	qpqual_cost;
	Cost		cpu_per_tuple;
	List	   *remote_conds;
	List	   *param_conds;
	List	   *local_conds;
	List	   *param_numbers;

	/*
	 * We use PgFdwRelationInfo to pass various information to subsequent
	 * functions.
	 */
	fpinfo = palloc0(sizeof(PgFdwRelationInfo));
	initStringInfo(&fpinfo->sql);
	sql = &fpinfo->sql;

	/*
	 * Determine whether we use remote estimate or not.  Note that per-table
	 * setting overrides per-server setting.
	 */
	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);
	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_explain") == 0)
		{
			use_remote_explain = defGetBoolean(def);
			break;
		}
	}
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_explain") == 0)
		{
			use_remote_explain = defGetBoolean(def);
			break;
		}
	}

	/*
	 * Construct remote query which consists of SELECT, FROM, and WHERE
	 * clauses.  Conditions which contain any Param node are excluded because
	 * placeholder can't be used in EXPLAIN statement.  Such conditions are
	 * appended later.
	 */
	classifyConditions(root, baserel, &remote_conds, &param_conds,
					   &local_conds, &param_numbers);
	deparseSimpleSql(sql, root, baserel, local_conds);
	if (list_length(remote_conds) > 0)
		appendWhereClause(sql, true, remote_conds, root);

	/*
	 * If the table or the server is configured to use remote EXPLAIN, connect
	 * to the foreign server and execute EXPLAIN with the quals that don't
	 * contain any Param nodes.  Otherwise, estimate rows using whatever
	 * statistics we have locally, in a way similar to ordinary tables.
	 */
	if (use_remote_explain)
	{
		RangeTblEntry *rte;
		Oid			userid;
		UserMapping *user;
		PGconn	   *conn;

		/*
		 * Identify which user to do the remote access as.	This should match
		 * what ExecCheckRTEPerms() does.  If we fail due to lack of
		 * permissions, the query would have failed at runtime anyway.
		 */
		rte = planner_rt_fetch(baserel->relid, root);
		userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

		user = GetUserMapping(userid, server->serverid);
		conn = GetConnection(server, user);
		get_remote_estimate(sql->data, conn, &rows, &width,
							&startup_cost, &total_cost);
		ReleaseConnection(conn);

		/*
		 * Estimate selectivity of conditions which were not used in remote
		 * EXPLAIN by calling clauselist_selectivity().  The best we can do
		 * for these conditions is to estimate selectivity on the basis of
		 * local statistics.
		 */
		sel = clauselist_selectivity(root, param_conds,
									 baserel->relid, JOIN_INNER, NULL);
		sel *= clauselist_selectivity(root, local_conds,
									  baserel->relid, JOIN_INNER, NULL);

		/*
		 * Add in the eval cost of those conditions, too.
		 */
		cost_qual_eval(&qpqual_cost, param_conds, root);
		startup_cost += qpqual_cost.startup;
		total_cost += qpqual_cost.per_tuple * rows;
		cost_qual_eval(&qpqual_cost, local_conds, root);
		startup_cost += qpqual_cost.startup;
		total_cost += qpqual_cost.per_tuple * rows;

		/* Report estimated numbers to planner. */
		baserel->rows = clamp_row_est(rows * sel);
		baserel->width = width;
	}
	else
	{
		/*
		 * Estimate rows from the result of the last ANALYZE, using all
		 * conditions specified in original query.
		 *
		 * If the foreign table has never been ANALYZEd, it will have relpages
		 * and reltuples equal to zero, which most likely has nothing to do
		 * with reality.  We can't do a whole lot about that if we're not
		 * allowed to consult the remote server, but we can use a hack similar
		 * to plancat.c's treatment of empty relations: use a minimum size
		 * estimate of 10 pages, and divide by the column-datatype-based width
		 * estimate to get the corresponding number of tuples.
		 */
		if (baserel->pages == 0 && baserel->tuples == 0)
		{
			baserel->pages = 10;
			baserel->tuples =
				(10 * BLCKSZ) / (baserel->width + sizeof(HeapTupleHeaderData));
		}

		set_baserel_size_estimates(root, baserel);

		/* Cost as though this were a seqscan, which is pessimistic. */
		startup_cost = 0;
		run_cost = 0;
		run_cost += seq_page_cost * baserel->pages;

		startup_cost += baserel->baserestrictcost.startup;
		cpu_per_tuple = cpu_tuple_cost + baserel->baserestrictcost.per_tuple;
		run_cost += cpu_per_tuple * baserel->tuples;

		total_cost = startup_cost + run_cost;
	}

	/*
	 * Finish deparsing remote query by adding conditions which were unusable
	 * in remote EXPLAIN since they contain Param nodes.
	 */
	if (list_length(param_conds) > 0)
		appendWhereClause(sql, !(list_length(remote_conds) > 0), param_conds,
						  root);

	/*
	 * Store obtained information into FDW-private area of RelOptInfo so it's
	 * available to subsequent functions.
	 */
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;
	fpinfo->remote_conds = remote_conds;
	fpinfo->param_conds = param_conds;
	fpinfo->local_conds = local_conds;
	fpinfo->param_numbers = param_numbers;
	fpinfo->table = table;
	fpinfo->server = server;
	baserel->fdw_private = (void *) fpinfo;
}

/*
 * postgresGetForeignPaths
 *		Create possible scan paths for a scan on the foreign table
 */
static void
postgresGetForeignPaths(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) baserel->fdw_private;
	ForeignPath *path;
	ListCell   *lc;
	double		fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
	double		fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;

	/*
	 * Check for user override of fdw_startup_cost, fdw_tuple_cost values
	 */
	foreach(lc, fpinfo->server->options)
	{
		DefElem    *d = (DefElem *) lfirst(lc);

		if (strcmp(d->defname, "fdw_startup_cost") == 0)
			fdw_startup_cost = strtod(defGetString(d), NULL);
		else if (strcmp(d->defname, "fdw_tuple_cost") == 0)
			fdw_tuple_cost = strtod(defGetString(d), NULL);
	}

	/*
	 * We have cost values which are estimated on remote side, so adjust them
	 * for better estimate which respect various stuffs to complete the scan,
	 * such as sending query, transferring result, and local filtering.
	 */
	startup_cost = fpinfo->startup_cost;
	total_cost = fpinfo->total_cost;

	/*----------
	 * Adjust costs with factors of the corresponding foreign server:
	 *	 - add cost to establish connection to both startup and total
	 *	 - add cost to manipulate on remote, and transfer result to total
	 *	 - add cost to manipulate tuples on local side to total
	 *----------
	 */
	startup_cost += fdw_startup_cost;
	total_cost += fdw_startup_cost;
	total_cost += fdw_tuple_cost * baserel->rows;
	total_cost += cpu_tuple_cost * baserel->rows;

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwPrivateIndex, above.
	 */
	fdw_private = list_make2(makeString(fpinfo->sql.data),
							 fpinfo->param_numbers);

	/*
	 * Create simplest ForeignScan path node and add it to baserel.  This path
	 * corresponds to SeqScan path of regular tables (though depending on what
	 * baserestrict conditions we were able to send to remote, there might
	 * actually be an indexscan happening there).
	 */
	path = create_foreignscan_path(root, baserel,
								   baserel->rows,
								   startup_cost,
								   total_cost,
								   NIL, /* no pathkeys */
								   NULL,		/* no outer rel either */
								   fdw_private);
	add_path(baserel, (Path *) path);

	/*
	 * XXX We can consider sorted path or parameterized path here if we know
	 * that foreign table is indexed on remote end.  For this purpose, we
	 * might have to support FOREIGN INDEX to represent possible sets of sort
	 * keys and/or filtering.  Or we could just try some join conditions and
	 * see if remote side estimates using them as markedly cheaper.  Note that
	 * executor functions need work to support internal Params before we can
	 * try generating any parameterized paths, though.
	 */
}

/*
 * postgresGetForeignPlan
 *		Create ForeignScan plan node which implements selected best path
 */
static ForeignScan *
postgresGetForeignPlan(PlannerInfo *root,
					   RelOptInfo *baserel,
					   Oid foreigntableid,
					   ForeignPath *best_path,
					   List *tlist,
					   List *scan_clauses)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) baserel->fdw_private;
	Index		scan_relid = baserel->relid;
	List	   *fdw_private = best_path->fdw_private;
	List	   *remote_exprs = NIL;
	List	   *local_exprs = NIL;
	ListCell   *lc;

	/*
	 * Separate the scan_clauses into those that can be executed remotely and
	 * those that can't.  For now, we accept only remote clauses that were
	 * previously determined to be safe by classifyClauses (so, only
	 * baserestrictinfo clauses can be used that way).
	 *
	 * This code must match "extract_actual_clauses(scan_clauses, false)"
	 * except for the additional decision about remote versus local execution.
	 */
	foreach(lc, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(IsA(rinfo, RestrictInfo));

		/* Ignore any pseudoconstants, they're dealt with elsewhere */
		if (rinfo->pseudoconstant)
			continue;

		/* Either simple or parameterized remote clauses are OK now */
		if (list_member_ptr(fpinfo->remote_conds, rinfo) ||
			list_member_ptr(fpinfo->param_conds, rinfo))
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		else
			local_exprs = lappend(local_exprs, rinfo->clause);
	}

	/*
	 * Create the ForeignScan node from target list, local filtering
	 * expressions, remote filtering expressions, and FDW private information.
	 *
	 * Note that the remote_exprs are stored in the fdw_exprs field of the
	 * finished plan node; we can't keep them in private state because then
	 * they wouldn't be subject to later planner processing.
	 *
	 * XXX Currently, the remote_exprs aren't actually used at runtime, so we
	 * don't need to store them at all.  But we'll keep this behavior for a
	 * little while for debugging reasons.
	 */
	return make_foreignscan(tlist,
							local_exprs,
							scan_relid,
							remote_exprs,
							fdw_private);
}

/*
 * postgresExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
postgresExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	List	   *fdw_private;
	char	   *sql;

	if (es->verbose)
	{
		fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;
		sql = strVal(list_nth(fdw_private, FdwPrivateSelectSql));
		ExplainPropertyText("Remote SQL", sql, es);
	}
}

/*
 * postgresBeginForeignScan
 *		Initiate an executor scan of a foreign PostgreSQL table.
 */
static void
postgresBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	PgFdwExecutionState *festate;
	RangeTblEntry *rte;
	Oid			userid;
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;
	List	   *param_numbers;
	int			numParams;
	int			i;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * We'll save private state in node->fdw_state.
	 */
	festate = (PgFdwExecutionState *) palloc0(sizeof(PgFdwExecutionState));
	node->fdw_state = (void *) festate;

	/*
	 * Identify which user to do the remote access as.	This should match what
	 * ExecCheckRTEPerms() does.
	 */
	rte = rt_fetch(fsplan->scan.scanrelid, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	festate->rel = node->ss.ss_currentRelation;
	table = GetForeignTable(RelationGetRelid(festate->rel));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	festate->conn = GetConnection(server, user);

	/* Assign a unique ID for my cursor */
	festate->cursor_number = GetCursorNumber(festate->conn);
	festate->cursor_exists = false;

	/* Get private info created by planner functions. */
	festate->fdw_private = fsplan->fdw_private;

	/* Create contexts for batches of tuples and per-tuple temp workspace. */
	festate->batch_cxt = AllocSetContextCreate(estate->es_query_cxt,
											   "postgres_fdw tuple data",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
	festate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_MINSIZE,
											  ALLOCSET_SMALL_INITSIZE,
											  ALLOCSET_SMALL_MAXSIZE);

	/* Get info we'll need for data conversion. */
	festate->attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(festate->rel));

	/*
	 * Allocate buffer for query parameters, if the remote conditions use any.
	 *
	 * We use a parameter slot for each PARAM_EXTERN parameter, even though
	 * not all of them may get sent to the remote server.  This allows us to
	 * refer to Params by their original number rather than remapping, and it
	 * doesn't cost much.  Slots that are not actually used get filled with
	 * null values that are arbitrarily marked as being of type int4.
	 */
	param_numbers = (List *)
		list_nth(festate->fdw_private, FdwPrivateExternParamIds);
	if (param_numbers != NIL)
	{
		ParamListInfo params = estate->es_param_list_info;

		numParams = params ? params->numParams : 0;
	}
	else
		numParams = 0;
	festate->numParams = numParams;
	if (numParams > 0)
	{
		/* we initially fill all slots with value = NULL, type = int4 */
		festate->param_types = (Oid *) palloc(numParams * sizeof(Oid));
		festate->param_values = (const char **) palloc0(numParams * sizeof(char *));
		for (i = 0; i < numParams; i++)
			festate->param_types[i] = INT4OID;
	}
	else
	{
		festate->param_types = NULL;
		festate->param_values = NULL;
	}
	festate->extparams_done = false;
}

/*
 * postgresIterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 */
static TupleTableSlot *
postgresIterateForeignScan(ForeignScanState *node)
{
	PgFdwExecutionState *festate = (PgFdwExecutionState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	/*
	 * If this is the first call after Begin or ReScan, we need to create the
	 * cursor on the remote side.
	 */
	if (!festate->cursor_exists)
		create_cursor(node);

	/*
	 * Get some more tuples, if we've run out.
	 */
	if (festate->next_tuple >= festate->num_tuples)
	{
		/* No point in another fetch if we already detected EOF, though. */
		if (!festate->eof_reached)
			fetch_more_data(node);
		/* If we didn't get any tuples, must be end of data. */
		if (festate->next_tuple >= festate->num_tuples)
			return ExecClearTuple(slot);
	}

	/*
	 * Return the next tuple.
	 */
	ExecStoreTuple(festate->tuples[festate->next_tuple++],
				   slot,
				   InvalidBuffer,
				   false);

	return slot;
}

/*
 * postgresReScanForeignScan
 *		Restart the scan.
 */
static void
postgresReScanForeignScan(ForeignScanState *node)
{
	PgFdwExecutionState *festate = (PgFdwExecutionState *) node->fdw_state;
	char		sql[64];
	PGresult   *res;

	/*
	 * Note: we assume that PARAM_EXTERN params don't change over the life of
	 * the query, so no need to reset extparams_done.
	 */

	/* If we haven't created the cursor yet, nothing to do. */
	if (!festate->cursor_exists)
		return;

	/*
	 * If any internal parameters affecting this node have changed, we'd
	 * better destroy and recreate the cursor.	Otherwise, rewinding it should
	 * be good enough.	If we've only fetched zero or one batch, we needn't
	 * even rewind the cursor, just rescan what we have.
	 */
	if (node->ss.ps.chgParam != NULL)
	{
		festate->cursor_exists = false;
		snprintf(sql, sizeof(sql), "CLOSE c%u",
				 festate->cursor_number);
	}
	else if (festate->fetch_ct_2 > 1)
	{
		snprintf(sql, sizeof(sql), "MOVE BACKWARD ALL IN c%u",
				 festate->cursor_number);
	}
	else
	{
		/* Easy: just rescan what we already have in memory, if anything */
		festate->next_tuple = 0;
		return;
	}

	/*
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQexec(festate->conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, true, sql);
	PQclear(res);

	/* Now force a fresh FETCH. */
	festate->tuples = NULL;
	festate->num_tuples = 0;
	festate->next_tuple = 0;
	festate->fetch_ct_2 = 0;
	festate->eof_reached = false;
}

/*
 * postgresEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
postgresEndForeignScan(ForeignScanState *node)
{
	PgFdwExecutionState *festate = (PgFdwExecutionState *) node->fdw_state;

	/* if festate is NULL, we are in EXPLAIN; nothing to do */
	if (festate == NULL)
		return;

	/* Close the cursor if open, to prevent accumulation of cursors */
	if (festate->cursor_exists)
		close_cursor(festate->conn, festate->cursor_number);

	/* Release remote connection */
	ReleaseConnection(festate->conn);
	festate->conn = NULL;

	/* MemoryContexts will be deleted automatically. */
}

/*
 * Estimate costs of executing given SQL statement.
 */
static void
get_remote_estimate(const char *sql, PGconn *conn,
					double *rows, int *width,
					Cost *startup_cost, Cost *total_cost)
{
	PGresult   *volatile res = NULL;

	/* PGresult must be released before leaving this function. */
	PG_TRY();
	{
		StringInfoData buf;
		char	   *line;
		char	   *p;
		int			n;

		/*
		 * Execute EXPLAIN remotely on given SQL statement.
		 */
		initStringInfo(&buf);
		appendStringInfo(&buf, "EXPLAIN %s", sql);
		res = PQexec(conn, buf.data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, false, buf.data);

		/*
		 * Extract cost numbers for topmost plan node.	Note we search for a
		 * left paren from the end of the line to avoid being confused by
		 * other uses of parentheses.
		 */
		line = PQgetvalue(res, 0, 0);
		p = strrchr(line, '(');
		if (p == NULL)
			elog(ERROR, "could not interpret EXPLAIN output: \"%s\"", line);
		n = sscanf(p, "(cost=%lf..%lf rows=%lf width=%d)",
				   startup_cost, total_cost, rows, width);
		if (n != 4)
			elog(ERROR, "could not interpret EXPLAIN output: \"%s\"", line);

		PQclear(res);
		res = NULL;
	}
	PG_CATCH();
	{
		if (res)
			PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Create cursor for node's query with current parameter values.
 */
static void
create_cursor(ForeignScanState *node)
{
	PgFdwExecutionState *festate = (PgFdwExecutionState *) node->fdw_state;
	int			numParams = festate->numParams;
	Oid		   *types = festate->param_types;
	const char **values = festate->param_values;
	PGconn	   *conn = festate->conn;
	char	   *sql;
	StringInfoData buf;
	PGresult   *res;

	/*
	 * Construct array of external parameter values in text format.  Since
	 * there might be random unconvertible stuff in the ParamExternData array,
	 * take care to convert only values we actually need.
	 *
	 * Note that we leak the memory for the value strings until end of query;
	 * this doesn't seem like a big problem, and in any case we might need to
	 * recreate the cursor after a rescan, so we could need to re-use the
	 * values anyway.
	 */
	if (numParams > 0 && !festate->extparams_done)
	{
		ParamListInfo params = node->ss.ps.state->es_param_list_info;
		List	   *param_numbers;
		ListCell   *lc;

		param_numbers = (List *)
			list_nth(festate->fdw_private, FdwPrivateExternParamIds);
		foreach(lc, param_numbers)
		{
			int			paramno = lfirst_int(lc);
			ParamExternData *prm = &params->params[paramno - 1];

			/* give hook a chance in case parameter is dynamic */
			if (!OidIsValid(prm->ptype) && params->paramFetch != NULL)
				params->paramFetch(params, paramno);

			/*
			 * Force the remote server to infer a type for this parameter.
			 * Since we explicitly cast every parameter (see deparse.c), the
			 * "inference" is trivial and will produce the desired result.
			 * This allows us to avoid assuming that the remote server has the
			 * same OIDs we do for the parameters' types.
			 *
			 * We'd not need to pass a type array to PQexecParams at all,
			 * except that there may be unused holes in the array, which
			 * will have to be filled with something or the remote server will
			 * complain.  We arbitrarily set them to INT4OID earlier.
			 */
			types[paramno - 1] = InvalidOid;

			/*
			 * Get string representation of each parameter value by invoking
			 * type-specific output function, unless the value is null.
			 */
			if (prm->isnull)
				values[paramno - 1] = NULL;
			else
			{
				Oid			out_func;
				bool		isvarlena;

				getTypeOutputInfo(prm->ptype, &out_func, &isvarlena);
				values[paramno - 1] = OidOutputFunctionCall(out_func,
															prm->value);
			}
		}
		festate->extparams_done = true;
	}

	/* Construct the DECLARE CURSOR command */
	sql = strVal(list_nth(festate->fdw_private, FdwPrivateSelectSql));
	initStringInfo(&buf);
	appendStringInfo(&buf, "DECLARE c%u CURSOR FOR\n%s",
					 festate->cursor_number, sql);

	/*
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQexecParams(conn, buf.data, numParams, types, values,
					   NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, true, sql);
	PQclear(res);

	/* Mark the cursor as created, and show no tuples have been retrieved */
	festate->cursor_exists = true;
	festate->tuples = NULL;
	festate->num_tuples = 0;
	festate->next_tuple = 0;
	festate->fetch_ct_2 = 0;
	festate->eof_reached = false;

	/* Clean up */
	pfree(buf.data);
}

/*
 * Fetch some more rows from the node's cursor.
 */
static void
fetch_more_data(ForeignScanState *node)
{
	PgFdwExecutionState *festate = (PgFdwExecutionState *) node->fdw_state;
	PGresult   *volatile res = NULL;
	MemoryContext oldcontext;

	/*
	 * We'll store the tuples in the batch_cxt.  First, flush the previous
	 * batch.
	 */
	festate->tuples = NULL;
	MemoryContextReset(festate->batch_cxt);
	oldcontext = MemoryContextSwitchTo(festate->batch_cxt);

	/* PGresult must be released before leaving this function. */
	PG_TRY();
	{
		PGconn	   *conn = festate->conn;
		char		sql[64];
		int			fetch_size;
		int			numrows;
		int			i;

		/* The fetch size is arbitrary, but shouldn't be enormous. */
		fetch_size = 100;

		snprintf(sql, sizeof(sql), "FETCH %d FROM c%u",
				 fetch_size, festate->cursor_number);

		res = PQexec(conn, sql);
		/* On error, report the original query, not the FETCH. */
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, false,
							   strVal(list_nth(festate->fdw_private,
											   FdwPrivateSelectSql)));

		/* Convert the data into HeapTuples */
		numrows = PQntuples(res);
		festate->tuples = (HeapTuple *) palloc0(numrows * sizeof(HeapTuple));
		festate->num_tuples = numrows;
		festate->next_tuple = 0;

		for (i = 0; i < numrows; i++)
		{
			festate->tuples[i] =
				make_tuple_from_result_row(res, i,
										   festate->rel,
										   festate->attinmeta,
										   festate->temp_cxt);
		}

		/* Update fetch_ct_2 */
		if (festate->fetch_ct_2 < 2)
			festate->fetch_ct_2++;

		/* Must be EOF if we didn't get as many tuples as we asked for. */
		festate->eof_reached = (numrows < fetch_size);

		PQclear(res);
		res = NULL;
	}
	PG_CATCH();
	{
		if (res)
			PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Utility routine to close a cursor.
 */
static void
close_cursor(PGconn *conn, unsigned int cursor_number)
{
	char		sql[64];
	PGresult   *res;

	snprintf(sql, sizeof(sql), "CLOSE c%u", cursor_number);

	/*
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQexec(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, true, sql);
	PQclear(res);
}

/*
 * postgresAnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 */
static bool
postgresAnalyzeForeignTable(Relation relation,
							AcquireSampleRowsFunc *func,
							BlockNumber *totalpages)
{
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;
	PGconn	   *conn;
	StringInfoData sql;
	PGresult   *volatile res = NULL;

	/* Return the row-analysis function pointer */
	*func = postgresAcquireSampleRowsFunc;

	/*
	 * Now we have to get the number of pages.  It's annoying that the ANALYZE
	 * API requires us to return that now, because it forces some duplication
	 * of effort between this routine and postgresAcquireSampleRowsFunc.  But
	 * it's probably not worth redefining that API at this point.
	 */

	/*
	 * Get the connection to use.  We do the remote access as the table's
	 * owner, even if the ANALYZE was started by some other user.
	 */
	table = GetForeignTable(RelationGetRelid(relation));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(relation->rd_rel->relowner, server->serverid);
	conn = GetConnection(server, user);

	/*
	 * Construct command to get page count for relation.
	 */
	initStringInfo(&sql);
	deparseAnalyzeSizeSql(&sql, relation);

	/* In what follows, do not risk leaking any PGresults. */
	PG_TRY();
	{
		res = PQexec(conn, sql.data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, false, sql.data);

		if (PQntuples(res) != 1 || PQnfields(res) != 1)
			elog(ERROR, "unexpected result from deparseAnalyzeSizeSql query");
		*totalpages = strtoul(PQgetvalue(res, 0, 0), NULL, 10);

		PQclear(res);
		res = NULL;
	}
	PG_CATCH();
	{
		if (res)
			PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	ReleaseConnection(conn);

	return true;
}

/*
 * Acquire a random sample of rows from foreign table managed by postgres_fdw.
 *
 * We fetch the whole table from the remote side and pick out some sample rows.
 *
 * Selected rows are returned in the caller-allocated array rows[],
 * which must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also count the total number of rows in the table and return it into
 * *totalrows.	Note that *totaldeadrows is always set to 0.
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the table.  Therefore, correlation estimates derived later
 * may be meaningless, but it's OK because we don't use the estimates
 * currently (the planner only pays attention to correlation for indexscans).
 */
static int
postgresAcquireSampleRowsFunc(Relation relation, int elevel,
							  HeapTuple *rows, int targrows,
							  double *totalrows,
							  double *totaldeadrows)
{
	PgFdwAnalyzeState astate;
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;
	PGconn	   *conn;
	unsigned int cursor_number;
	StringInfoData sql;
	PGresult   *volatile res = NULL;

	/* Initialize workspace state */
	astate.rel = relation;
	astate.attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(relation));

	astate.rows = rows;
	astate.targrows = targrows;
	astate.numrows = 0;
	astate.samplerows = 0;
	astate.rowstoskip = -1;		/* -1 means not set yet */
	astate.rstate = anl_init_selection_state(targrows);

	/* Remember ANALYZE context, and create a per-tuple temp context */
	astate.anl_cxt = CurrentMemoryContext;
	astate.temp_cxt = AllocSetContextCreate(CurrentMemoryContext,
											"postgres_fdw temporary data",
											ALLOCSET_SMALL_MINSIZE,
											ALLOCSET_SMALL_INITSIZE,
											ALLOCSET_SMALL_MAXSIZE);

	/*
	 * Get the connection to use.  We do the remote access as the table's
	 * owner, even if the ANALYZE was started by some other user.
	 */
	table = GetForeignTable(RelationGetRelid(relation));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(relation->rd_rel->relowner, server->serverid);
	conn = GetConnection(server, user);

	/*
	 * Construct cursor that retrieves whole rows from remote.
	 */
	cursor_number = GetCursorNumber(conn);
	initStringInfo(&sql);
	appendStringInfo(&sql, "DECLARE c%u CURSOR FOR ", cursor_number);
	deparseAnalyzeSql(&sql, relation);

	/* In what follows, do not risk leaking any PGresults. */
	PG_TRY();
	{
		res = PQexec(conn, sql.data);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			pgfdw_report_error(ERROR, res, false, sql.data);
		PQclear(res);
		res = NULL;

		/* Retrieve and process rows a batch at a time. */
		for (;;)
		{
			char		fetch_sql[64];
			int			fetch_size;
			int			numrows;
			int			i;

			/* Allow users to cancel long query */
			CHECK_FOR_INTERRUPTS();

			/*
			 * XXX possible future improvement: if rowstoskip is large, we
			 * could issue a MOVE rather than physically fetching the rows,
			 * then just adjust rowstoskip and samplerows appropriately.
			 */

			/* The fetch size is arbitrary, but shouldn't be enormous. */
			fetch_size = 100;

			/* Fetch some rows */
			snprintf(fetch_sql, sizeof(fetch_sql), "FETCH %d FROM c%u",
					 fetch_size, cursor_number);

			res = PQexec(conn, fetch_sql);
			/* On error, report the original query, not the FETCH. */
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
				pgfdw_report_error(ERROR, res, false, sql.data);

			/* Process whatever we got. */
			numrows = PQntuples(res);
			for (i = 0; i < numrows; i++)
				analyze_row_processor(res, i, &astate);

			PQclear(res);
			res = NULL;

			/* Must be EOF if we didn't get all the rows requested. */
			if (numrows < fetch_size)
				break;
		}

		/* Close the cursor, just to be tidy. */
		close_cursor(conn, cursor_number);
	}
	PG_CATCH();
	{
		if (res)
			PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();

	ReleaseConnection(conn);

	/* We assume that we have no dead tuple. */
	*totaldeadrows = 0.0;

	/* We've retrieved all living tuples from foreign server. */
	*totalrows = astate.samplerows;

	/*
	 * Emit some interesting relation info
	 */
	ereport(elevel,
			(errmsg("\"%s\": table contains %.0f rows, %d rows in sample",
					RelationGetRelationName(relation),
					astate.samplerows, astate.numrows)));

	return astate.numrows;
}

/*
 * Collect sample rows from the result of query.
 *	 - Use all tuples in sample until target # of samples are collected.
 *	 - Subsequently, replace already-sampled tuples randomly.
 */
static void
analyze_row_processor(PGresult *res, int row, PgFdwAnalyzeState *astate)
{
	int			targrows = astate->targrows;
	int			pos;			/* array index to store tuple in */
	MemoryContext oldcontext;

	/* Always increment sample row counter. */
	astate->samplerows += 1;

	/*
	 * Determine the slot where this sample row should be stored.  Set pos to
	 * negative value to indicate the row should be skipped.
	 */
	if (astate->numrows < targrows)
	{
		/* First targrows rows are always included into the sample */
		pos = astate->numrows++;
	}
	else
	{
		/*
		 * Now we start replacing tuples in the sample until we reach the end
		 * of the relation.  Same algorithm as in acquire_sample_rows in
		 * analyze.c; see Jeff Vitter's paper.
		 */
		if (astate->rowstoskip < 0)
			astate->rowstoskip = anl_get_next_S(astate->samplerows, targrows,
												&astate->rstate);

		if (astate->rowstoskip <= 0)
		{
			/* Choose a random reservoir element to replace. */
			pos = (int) (targrows * anl_random_fract());
			Assert(pos >= 0 && pos < targrows);
			heap_freetuple(astate->rows[pos]);
		}
		else
		{
			/* Skip this tuple. */
			pos = -1;
		}

		astate->rowstoskip -= 1;
	}

	if (pos >= 0)
	{
		/*
		 * Create sample tuple from current result row, and store it in the
		 * position determined above.  The tuple has to be created in anl_cxt.
		 */
		oldcontext = MemoryContextSwitchTo(astate->anl_cxt);

		astate->rows[pos] = make_tuple_from_result_row(res, row,
													   astate->rel,
													   astate->attinmeta,
													   astate->temp_cxt);

		MemoryContextSwitchTo(oldcontext);
	}
}

/*
 * Create a tuple from the specified row of the PGresult.
 *
 * rel is the local representation of the foreign table, attinmeta is
 * conversion data for the rel's tupdesc, and temp_context is a working
 * context that can be reset after each tuple.
 */
static HeapTuple
make_tuple_from_result_row(PGresult *res,
						   int row,
						   Relation rel,
						   AttInMetadata *attinmeta,
						   MemoryContext temp_context)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Form_pg_attribute *attrs = tupdesc->attrs;
	Datum	   *values;
	bool	   *nulls;
	ConversionLocation errpos;
	ErrorContextCallback errcallback;
	MemoryContext oldcontext;
	int			i;
	int			j;

	Assert(row < PQntuples(res));

	/*
	 * Do the following work in a temp context that we reset after each tuple.
	 * This cleans up not only the data we have direct access to, but any
	 * cruft the I/O functions might leak.
	 */
	oldcontext = MemoryContextSwitchTo(temp_context);

	values = (Datum *) palloc(tupdesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));

	/*
	 * Set up and install callback to report where conversion error occurs.
	 */
	errpos.rel = rel;
	errpos.cur_attno = 0;
	errcallback.callback = conversion_error_callback;
	errcallback.arg = (void *) &errpos;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * i indexes columns in the relation, j indexes columns in the PGresult.
	 * We assume dropped columns are not represented in the PGresult.
	 */
	for (i = 0, j = 0; i < tupdesc->natts; i++)
	{
		char	   *valstr;

		/* skip dropped columns. */
		if (attrs[i]->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		/* convert value to internal representation */
		if (PQgetisnull(res, row, j))
		{
			valstr = NULL;
			nulls[i] = true;
		}
		else
		{
			valstr = PQgetvalue(res, row, j);
			nulls[i] = false;
		}

		/* Note: apply the input function even to nulls, to support domains */
		errpos.cur_attno = i + 1;
		values[i] = InputFunctionCall(&attinmeta->attinfuncs[i],
									  valstr,
									  attinmeta->attioparams[i],
									  attinmeta->atttypmods[i]);
		errpos.cur_attno = 0;

		j++;
	}

	/* Uninstall error context callback. */
	error_context_stack = errcallback.previous;

	/* check result and tuple descriptor have the same number of columns */
	if (j != PQnfields(res))
		elog(ERROR, "remote query result does not match the foreign table");

	/*
	 * Build the result tuple in caller's memory context.
	 */
	MemoryContextSwitchTo(oldcontext);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/* Clean up */
	MemoryContextReset(temp_context);

	return tuple;
}

/*
 * Callback function which is called when error occurs during column value
 * conversion.	Print names of column and relation.
 */
static void
conversion_error_callback(void *arg)
{
	ConversionLocation *errpos = (ConversionLocation *) arg;
	TupleDesc	tupdesc = RelationGetDescr(errpos->rel);

	if (errpos->cur_attno > 0 && errpos->cur_attno <= tupdesc->natts)
		errcontext("column \"%s\" of foreign table \"%s\"",
				   NameStr(tupdesc->attrs[errpos->cur_attno - 1]->attname),
				   RelationGetRelationName(errpos->rel));
}
