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
#include "access/sysattr.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
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
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * We store various information in ForeignScan.fdw_private to pass it from
 * planner to executor.  Specifically there is:
 *
 * 1) SELECT statement text to be sent to the remote server
 * 2) IDs of PARAM_EXEC Params used in the SELECT statement
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().	For example, to get the SELECT statement:
 *		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
 */
enum FdwScanPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwScanPrivateSelectSql,
	/* Integer list of param IDs of PARAM_EXEC Params used in SQL stmt */
	FdwScanPrivateExternParamIds
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a postgres_fdw foreign table.  We store:
 *
 * 1) INSERT/UPDATE/DELETE statement text to be sent to the remote server
 * 2) Integer list of target attribute numbers for INSERT/UPDATE
 *	  (NIL for a DELETE)
 * 3) Boolean flag showing if there's a RETURNING clause
 */
enum FdwModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwModifyPrivateUpdateSql,
	/* Integer list of target attribute numbers for INSERT/UPDATE */
	FdwModifyPrivateTargetAttnums,
	/* has-returning flag (as an integer Value node) */
	FdwModifyPrivateHasReturning
};

/*
 * Execution state of a foreign scan using postgres_fdw.
 */
typedef struct PgFdwScanState
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
} PgFdwScanState;

/*
 * Execution state of a foreign insert/update/delete operation.
 */
typedef struct PgFdwModifyState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* for remote query execution */
	PGconn	   *conn;			/* connection for the scan */
	char	   *p_name;			/* name of prepared statement, if created */

	/* extracted fdw_private data */
	char	   *query;			/* text of INSERT/UPDATE/DELETE command */
	List	   *target_attrs;	/* list of target attribute numbers */
	bool		has_returning;	/* is there a RETURNING clause? */

	/* info about parameters for prepared statement */
	AttrNumber	ctidAttno;		/* attnum of input resjunk ctid column */
	int			p_nums;			/* number of parameters to transmit */
	FmgrInfo   *p_flinfo;		/* output conversion functions for them */

	/* working memory context */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} PgFdwModifyState;

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
static void postgresBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *postgresIterateForeignScan(ForeignScanState *node);
static void postgresReScanForeignScan(ForeignScanState *node);
static void postgresEndForeignScan(ForeignScanState *node);
static void postgresAddForeignUpdateTargets(Query *parsetree,
								RangeTblEntry *target_rte,
								Relation target_relation);
static List *postgresPlanForeignModify(PlannerInfo *root,
						  ModifyTable *plan,
						  Index resultRelation,
						  int subplan_index);
static void postgresBeginForeignModify(ModifyTableState *mtstate,
						   ResultRelInfo *resultRelInfo,
						   List *fdw_private,
						   int subplan_index,
						   int eflags);
static TupleTableSlot *postgresExecForeignInsert(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot);
static TupleTableSlot *postgresExecForeignUpdate(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot);
static TupleTableSlot *postgresExecForeignDelete(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot);
static void postgresEndForeignModify(EState *estate,
						 ResultRelInfo *resultRelInfo);
static void postgresExplainForeignScan(ForeignScanState *node,
						   ExplainState *es);
static void postgresExplainForeignModify(ModifyTableState *mtstate,
							 ResultRelInfo *rinfo,
							 List *fdw_private,
							 int subplan_index,
							 ExplainState *es);
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
static void prepare_foreign_modify(PgFdwModifyState *fmstate);
static const char **convert_prep_stmt_params(PgFdwModifyState *fmstate,
						 ItemPointer tupleid,
						 TupleTableSlot *slot);
static void store_returning_result(PgFdwModifyState *fmstate,
					   TupleTableSlot *slot, PGresult *res);
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

	/* Functions for scanning foreign tables */
	routine->GetForeignRelSize = postgresGetForeignRelSize;
	routine->GetForeignPaths = postgresGetForeignPaths;
	routine->GetForeignPlan = postgresGetForeignPlan;
	routine->BeginForeignScan = postgresBeginForeignScan;
	routine->IterateForeignScan = postgresIterateForeignScan;
	routine->ReScanForeignScan = postgresReScanForeignScan;
	routine->EndForeignScan = postgresEndForeignScan;

	/* Functions for updating foreign tables */
	routine->AddForeignUpdateTargets = postgresAddForeignUpdateTargets;
	routine->PlanForeignModify = postgresPlanForeignModify;
	routine->BeginForeignModify = postgresBeginForeignModify;
	routine->ExecForeignInsert = postgresExecForeignInsert;
	routine->ExecForeignUpdate = postgresExecForeignUpdate;
	routine->ExecForeignDelete = postgresExecForeignDelete;
	routine->EndForeignModify = postgresEndForeignModify;

	/* Support functions for EXPLAIN */
	routine->ExplainForeignScan = postgresExplainForeignScan;
	routine->ExplainForeignModify = postgresExplainForeignModify;

	/* Support functions for ANALYZE */
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
	bool		use_remote_estimate = false;
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
	Bitmapset  *attrs_used;
	ListCell   *lc;

	/*
	 * We use PgFdwRelationInfo to pass various information to subsequent
	 * functions.
	 */
	fpinfo = (PgFdwRelationInfo *) palloc0(sizeof(PgFdwRelationInfo));
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

		if (strcmp(def->defname, "use_remote_estimate") == 0)
		{
			use_remote_estimate = defGetBoolean(def);
			break;
		}
	}
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_estimate") == 0)
		{
			use_remote_estimate = defGetBoolean(def);
			break;
		}
	}

	/*
	 * Identify which restriction clauses can be sent to the remote server and
	 * which can't.  Conditions that are remotely executable but contain
	 * PARAM_EXTERN Params have to be treated separately because we can't use
	 * placeholders in remote EXPLAIN.
	 */
	classifyConditions(root, baserel, &remote_conds, &param_conds,
					   &local_conds, &param_numbers);

	/*
	 * Identify which attributes will need to be retrieved from the remote
	 * server.	These include all attrs needed for joins or final output, plus
	 * all attrs used in the local_conds.
	 */
	attrs_used = NULL;
	pull_varattnos((Node *) baserel->reltargetlist, baserel->relid,
				   &attrs_used);
	foreach(lc, local_conds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &attrs_used);
	}

	/*
	 * Construct remote query which consists of SELECT, FROM, and WHERE
	 * clauses.  For now, leave out the param_conds.
	 */
	deparseSelectSql(sql, root, baserel, attrs_used);
	if (remote_conds)
		appendWhereClause(sql, root, remote_conds, true);

	/*
	 * If the table or the server is configured to use remote estimates,
	 * connect to the foreign server and execute EXPLAIN with the quals that
	 * don't contain any Param nodes.  Otherwise, estimate rows using whatever
	 * statistics we have locally, in a way similar to ordinary tables.
	 */
	if (use_remote_estimate)
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
		conn = GetConnection(server, user, false);
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
	 * in remote EXPLAIN because they contain Param nodes.
	 */
	if (param_conds)
		appendWhereClause(sql, root, param_conds, (remote_conds == NIL));

	/*
	 * Add FOR UPDATE/SHARE if appropriate.  We apply locking during the
	 * initial row fetch, rather than later on as is done for local tables.
	 * The extra roundtrips involved in trying to duplicate the local
	 * semantics exactly don't seem worthwhile (see also comments for
	 * RowMarkType).
	 */
	if (baserel->relid == root->parse->resultRelation &&
		(root->parse->commandType == CMD_UPDATE ||
		 root->parse->commandType == CMD_DELETE))
	{
		/* Relation is UPDATE/DELETE target, so use FOR UPDATE */
		appendStringInfo(sql, " FOR UPDATE");
	}
	else
	{
		RowMarkClause *rc = get_parse_rowmark(root->parse, baserel->relid);

		if (rc)
		{
			/*
			 * Relation is specified as a FOR UPDATE/SHARE target, so handle
			 * that.
			 *
			 * For now, just ignore any [NO] KEY specification, since (a) it's
			 * not clear what that means for a remote table that we don't have
			 * complete information about, and (b) it wouldn't work anyway on
			 * older remote servers.  Likewise, we don't worry about NOWAIT.
			 */
			switch (rc->strength)
			{
				case LCS_FORKEYSHARE:
				case LCS_FORSHARE:
					appendStringInfo(sql, " FOR SHARE");
					break;
				case LCS_FORNOKEYUPDATE:
				case LCS_FORUPDATE:
					appendStringInfo(sql, " FOR UPDATE");
					break;
			}
		}
	}

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
	 * Items in the list must match enum FdwScanPrivateIndex, above.
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
 * postgresBeginForeignScan
 *		Initiate an executor scan of a foreign PostgreSQL table.
 */
static void
postgresBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	PgFdwScanState *fsstate;
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
	fsstate = (PgFdwScanState *) palloc0(sizeof(PgFdwScanState));
	node->fdw_state = (void *) fsstate;

	/*
	 * Identify which user to do the remote access as.	This should match what
	 * ExecCheckRTEPerms() does.
	 */
	rte = rt_fetch(fsplan->scan.scanrelid, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	fsstate->rel = node->ss.ss_currentRelation;
	table = GetForeignTable(RelationGetRelid(fsstate->rel));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	fsstate->conn = GetConnection(server, user, false);

	/* Assign a unique ID for my cursor */
	fsstate->cursor_number = GetCursorNumber(fsstate->conn);
	fsstate->cursor_exists = false;

	/* Get private info created by planner functions. */
	fsstate->fdw_private = fsplan->fdw_private;

	/* Create contexts for batches of tuples and per-tuple temp workspace. */
	fsstate->batch_cxt = AllocSetContextCreate(estate->es_query_cxt,
											   "postgres_fdw tuple data",
											   ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
	fsstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_MINSIZE,
											  ALLOCSET_SMALL_INITSIZE,
											  ALLOCSET_SMALL_MAXSIZE);

	/* Get info we'll need for data conversion. */
	fsstate->attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(fsstate->rel));

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
		list_nth(fsstate->fdw_private, FdwScanPrivateExternParamIds);
	if (param_numbers != NIL)
	{
		ParamListInfo params = estate->es_param_list_info;

		numParams = params ? params->numParams : 0;
	}
	else
		numParams = 0;
	fsstate->numParams = numParams;
	if (numParams > 0)
	{
		/* we initially fill all slots with value = NULL, type = int4 */
		fsstate->param_types = (Oid *) palloc(numParams * sizeof(Oid));
		fsstate->param_values = (const char **) palloc0(numParams * sizeof(char *));
		for (i = 0; i < numParams; i++)
			fsstate->param_types[i] = INT4OID;
	}
	else
	{
		fsstate->param_types = NULL;
		fsstate->param_values = NULL;
	}
	fsstate->extparams_done = false;
}

/*
 * postgresIterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 */
static TupleTableSlot *
postgresIterateForeignScan(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	/*
	 * If this is the first call after Begin or ReScan, we need to create the
	 * cursor on the remote side.
	 */
	if (!fsstate->cursor_exists)
		create_cursor(node);

	/*
	 * Get some more tuples, if we've run out.
	 */
	if (fsstate->next_tuple >= fsstate->num_tuples)
	{
		/* No point in another fetch if we already detected EOF, though. */
		if (!fsstate->eof_reached)
			fetch_more_data(node);
		/* If we didn't get any tuples, must be end of data. */
		if (fsstate->next_tuple >= fsstate->num_tuples)
			return ExecClearTuple(slot);
	}

	/*
	 * Return the next tuple.
	 */
	ExecStoreTuple(fsstate->tuples[fsstate->next_tuple++],
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
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	char		sql[64];
	PGresult   *res;

	/*
	 * Note: we assume that PARAM_EXTERN params don't change over the life of
	 * the query, so no need to reset extparams_done.
	 */

	/* If we haven't created the cursor yet, nothing to do. */
	if (!fsstate->cursor_exists)
		return;

	/*
	 * If any internal parameters affecting this node have changed, we'd
	 * better destroy and recreate the cursor.	Otherwise, rewinding it should
	 * be good enough.	If we've only fetched zero or one batch, we needn't
	 * even rewind the cursor, just rescan what we have.
	 */
	if (node->ss.ps.chgParam != NULL)
	{
		fsstate->cursor_exists = false;
		snprintf(sql, sizeof(sql), "CLOSE c%u",
				 fsstate->cursor_number);
	}
	else if (fsstate->fetch_ct_2 > 1)
	{
		snprintf(sql, sizeof(sql), "MOVE BACKWARD ALL IN c%u",
				 fsstate->cursor_number);
	}
	else
	{
		/* Easy: just rescan what we already have in memory, if anything */
		fsstate->next_tuple = 0;
		return;
	}

	/*
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQexec(fsstate->conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, true, sql);
	PQclear(res);

	/* Now force a fresh FETCH. */
	fsstate->tuples = NULL;
	fsstate->num_tuples = 0;
	fsstate->next_tuple = 0;
	fsstate->fetch_ct_2 = 0;
	fsstate->eof_reached = false;
}

/*
 * postgresEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
postgresEndForeignScan(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;

	/* if fsstate is NULL, we are in EXPLAIN; nothing to do */
	if (fsstate == NULL)
		return;

	/* Close the cursor if open, to prevent accumulation of cursors */
	if (fsstate->cursor_exists)
		close_cursor(fsstate->conn, fsstate->cursor_number);

	/* Release remote connection */
	ReleaseConnection(fsstate->conn);
	fsstate->conn = NULL;

	/* MemoryContexts will be deleted automatically. */
}

/*
 * postgresAddForeignUpdateTargets
 *		Add resjunk column(s) needed for update/delete on a foreign table
 */
static void
postgresAddForeignUpdateTargets(Query *parsetree,
								RangeTblEntry *target_rte,
								Relation target_relation)
{
	Var		   *var;
	const char *attrname;
	TargetEntry *tle;

	/*
	 * In postgres_fdw, what we need is the ctid, same as for a regular table.
	 */

	/* Make a Var representing the desired value */
	var = makeVar(parsetree->resultRelation,
				  SelfItemPointerAttributeNumber,
				  TIDOID,
				  -1,
				  InvalidOid,
				  0);

	/* Wrap it in a resjunk TLE with the right name ... */
	attrname = "ctid";

	tle = makeTargetEntry((Expr *) var,
						  list_length(parsetree->targetList) + 1,
						  pstrdup(attrname),
						  true);

	/* ... and add it to the query's targetlist */
	parsetree->targetList = lappend(parsetree->targetList, tle);
}

/*
 * postgresPlanForeignModify
 *		Plan an insert/update/delete operation on a foreign table
 *
 * Note: currently, the plan tree generated for UPDATE/DELETE will always
 * include a ForeignScan that retrieves ctids (using SELECT FOR UPDATE)
 * and then the ModifyTable node will have to execute individual remote
 * UPDATE/DELETE commands.	If there are no local conditions or joins
 * needed, it'd be better to let the scan node do UPDATE/DELETE RETURNING
 * and then do nothing at ModifyTable.	Room for future optimization ...
 */
static List *
postgresPlanForeignModify(PlannerInfo *root,
						  ModifyTable *plan,
						  Index resultRelation,
						  int subplan_index)
{
	CmdType		operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	StringInfoData sql;
	List	   *targetAttrs = NIL;
	List	   *returningList = NIL;

	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	rel = heap_open(rte->relid, NoLock);

	/*
	 * In an INSERT, we transmit all columns that are defined in the foreign
	 * table.  In an UPDATE, we transmit only columns that were explicitly
	 * targets of the UPDATE, so as to avoid unnecessary data transmission.
	 * (We can't do that for INSERT since we would miss sending default values
	 * for columns not listed in the source statement.)
	 */
	if (operation == CMD_INSERT)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = tupdesc->attrs[attnum - 1];

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		Bitmapset  *tmpset = bms_copy(rte->modifiedCols);
		AttrNumber	col;

		while ((col = bms_first_member(tmpset)) >= 0)
		{
			col += FirstLowInvalidHeapAttributeNumber;
			if (col <= InvalidAttrNumber)		/* shouldn't happen */
				elog(ERROR, "system-column update is not supported");
			targetAttrs = lappend_int(targetAttrs, col);
		}
	}

	/*
	 * Extract the relevant RETURNING list if any.
	 */
	if (plan->returningLists)
		returningList = (List *) list_nth(plan->returningLists, subplan_index);

	/*
	 * Construct the SQL command string.
	 */
	switch (operation)
	{
		case CMD_INSERT:
			deparseInsertSql(&sql, root, resultRelation, rel,
							 targetAttrs, returningList);
			break;
		case CMD_UPDATE:
			deparseUpdateSql(&sql, root, resultRelation, rel,
							 targetAttrs, returningList);
			break;
		case CMD_DELETE:
			deparseDeleteSql(&sql, root, resultRelation, rel,
							 returningList);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	heap_close(rel, NoLock);

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwModifyPrivateIndex, above.
	 */
	return list_make3(makeString(sql.data),
					  targetAttrs,
					  makeInteger((returningList != NIL)));
}

/*
 * postgresBeginForeignModify
 *		Begin an insert/update/delete operation on a foreign table
 */
static void
postgresBeginForeignModify(ModifyTableState *mtstate,
						   ResultRelInfo *resultRelInfo,
						   List *fdw_private,
						   int subplan_index,
						   int eflags)
{
	PgFdwModifyState *fmstate;
	EState	   *estate = mtstate->ps.state;
	CmdType		operation = mtstate->operation;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	RangeTblEntry *rte;
	Oid			userid;
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;
	AttrNumber	n_params;
	Oid			typefnoid;
	bool		isvarlena;
	ListCell   *lc;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  resultRelInfo->ri_FdwState
	 * stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Begin constructing PgFdwModifyState. */
	fmstate = (PgFdwModifyState *) palloc0(sizeof(PgFdwModifyState));
	fmstate->rel = rel;

	/*
	 * Identify which user to do the remote access as.	This should match what
	 * ExecCheckRTEPerms() does.
	 */
	rte = rt_fetch(resultRelInfo->ri_RangeTableIndex, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	table = GetForeignTable(RelationGetRelid(rel));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/* Open connection; report that we'll create a prepared statement. */
	fmstate->conn = GetConnection(server, user, true);
	fmstate->p_name = NULL;		/* prepared statement not made yet */

	/* Deconstruct fdw_private data. */
	fmstate->query = strVal(list_nth(fdw_private,
									 FdwModifyPrivateUpdateSql));
	fmstate->target_attrs = (List *) list_nth(fdw_private,
											  FdwModifyPrivateTargetAttnums);
	fmstate->has_returning = intVal(list_nth(fdw_private,
											 FdwModifyPrivateHasReturning));

	/* Create context for per-tuple temp workspace. */
	fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_MINSIZE,
											  ALLOCSET_SMALL_INITSIZE,
											  ALLOCSET_SMALL_MAXSIZE);

	/* Prepare for input conversion of RETURNING results. */
	if (fmstate->has_returning)
		fmstate->attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(rel));

	/* Prepare for output conversion of parameters used in prepared stmt. */
	n_params = list_length(fmstate->target_attrs) + 1;
	fmstate->p_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * n_params);
	fmstate->p_nums = 0;

	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		/* Find the ctid resjunk column in the subplan's result */
		Plan	   *subplan = mtstate->mt_plans[subplan_index]->plan;

		fmstate->ctidAttno = ExecFindJunkAttributeInTlist(subplan->targetlist,
														  "ctid");
		if (!AttributeNumberIsValid(fmstate->ctidAttno))
			elog(ERROR, "could not find junk ctid column");

		/* First transmittable parameter will be ctid */
		getTypeOutputInfo(TIDOID, &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
		fmstate->p_nums++;
	}

	if (operation == CMD_INSERT || operation == CMD_UPDATE)
	{
		/* Set up for remaining transmittable parameters */
		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = RelationGetDescr(rel)->attrs[attnum - 1];

			Assert(!attr->attisdropped);

			getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
			fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
			fmstate->p_nums++;
		}
	}

	Assert(fmstate->p_nums <= n_params);

	resultRelInfo->ri_FdwState = fmstate;
}

/*
 * postgresExecForeignInsert
 *		Insert one row into a foreign table
 */
static TupleTableSlot *
postgresExecForeignInsert(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;
	const char **p_values;
	PGresult   *res;
	int			n_rows;

	/* Set up the prepared statement on the remote server, if we didn't yet */
	if (!fmstate->p_name)
		prepare_foreign_modify(fmstate);

	/* Convert parameters needed by prepared statement to text form */
	p_values = convert_prep_stmt_params(fmstate, NULL, slot);

	/*
	 * Execute the prepared statement, and check for success.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQexecPrepared(fmstate->conn,
						 fmstate->p_name,
						 fmstate->p_nums,
						 p_values,
						 NULL,
						 NULL,
						 0);
	if (PQresultStatus(res) !=
		(fmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
		pgfdw_report_error(ERROR, res, true, fmstate->query);

	/* Check number of rows affected, and fetch RETURNING tuple if any */
	if (fmstate->has_returning)
	{
		n_rows = PQntuples(res);
		if (n_rows > 0)
			store_returning_result(fmstate, slot, res);
	}
	else
		n_rows = atoi(PQcmdTuples(res));

	/* And clean up */
	PQclear(res);

	MemoryContextReset(fmstate->temp_cxt);

	/* Return NULL if nothing was inserted on the remote end */
	return (n_rows > 0) ? slot : NULL;
}

/*
 * postgresExecForeignUpdate
 *		Update one row in a foreign table
 */
static TupleTableSlot *
postgresExecForeignUpdate(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;
	Datum		datum;
	bool		isNull;
	const char **p_values;
	PGresult   *res;
	int			n_rows;

	/* Set up the prepared statement on the remote server, if we didn't yet */
	if (!fmstate->p_name)
		prepare_foreign_modify(fmstate);

	/* Get the ctid that was passed up as a resjunk column */
	datum = ExecGetJunkAttribute(planSlot,
								 fmstate->ctidAttno,
								 &isNull);
	/* shouldn't ever get a null result... */
	if (isNull)
		elog(ERROR, "ctid is NULL");

	/* Convert parameters needed by prepared statement to text form */
	p_values = convert_prep_stmt_params(fmstate,
										(ItemPointer) DatumGetPointer(datum),
										slot);

	/*
	 * Execute the prepared statement, and check for success.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQexecPrepared(fmstate->conn,
						 fmstate->p_name,
						 fmstate->p_nums,
						 p_values,
						 NULL,
						 NULL,
						 0);
	if (PQresultStatus(res) !=
		(fmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
		pgfdw_report_error(ERROR, res, true, fmstate->query);

	/* Check number of rows affected, and fetch RETURNING tuple if any */
	if (fmstate->has_returning)
	{
		n_rows = PQntuples(res);
		if (n_rows > 0)
			store_returning_result(fmstate, slot, res);
	}
	else
		n_rows = atoi(PQcmdTuples(res));

	/* And clean up */
	PQclear(res);

	MemoryContextReset(fmstate->temp_cxt);

	/* Return NULL if nothing was updated on the remote end */
	return (n_rows > 0) ? slot : NULL;
}

/*
 * postgresExecForeignDelete
 *		Delete one row from a foreign table
 */
static TupleTableSlot *
postgresExecForeignDelete(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;
	Datum		datum;
	bool		isNull;
	const char **p_values;
	PGresult   *res;
	int			n_rows;

	/* Set up the prepared statement on the remote server, if we didn't yet */
	if (!fmstate->p_name)
		prepare_foreign_modify(fmstate);

	/* Get the ctid that was passed up as a resjunk column */
	datum = ExecGetJunkAttribute(planSlot,
								 fmstate->ctidAttno,
								 &isNull);
	/* shouldn't ever get a null result... */
	if (isNull)
		elog(ERROR, "ctid is NULL");

	/* Convert parameters needed by prepared statement to text form */
	p_values = convert_prep_stmt_params(fmstate,
										(ItemPointer) DatumGetPointer(datum),
										NULL);

	/*
	 * Execute the prepared statement, and check for success.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQexecPrepared(fmstate->conn,
						 fmstate->p_name,
						 fmstate->p_nums,
						 p_values,
						 NULL,
						 NULL,
						 0);
	if (PQresultStatus(res) !=
		(fmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
		pgfdw_report_error(ERROR, res, true, fmstate->query);

	/* Check number of rows affected, and fetch RETURNING tuple if any */
	if (fmstate->has_returning)
	{
		n_rows = PQntuples(res);
		if (n_rows > 0)
			store_returning_result(fmstate, slot, res);
	}
	else
		n_rows = atoi(PQcmdTuples(res));

	/* And clean up */
	PQclear(res);

	MemoryContextReset(fmstate->temp_cxt);

	/* Return NULL if nothing was deleted on the remote end */
	return (n_rows > 0) ? slot : NULL;
}

/*
 * postgresEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
postgresEndForeignModify(EState *estate,
						 ResultRelInfo *resultRelInfo)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;

	/* If fmstate is NULL, we are in EXPLAIN; nothing to do */
	if (fmstate == NULL)
		return;

	/* If we created a prepared statement, destroy it */
	if (fmstate->p_name)
	{
		char		sql[64];
		PGresult   *res;

		snprintf(sql, sizeof(sql), "DEALLOCATE %s", fmstate->p_name);

		/*
		 * We don't use a PG_TRY block here, so be careful not to throw error
		 * without releasing the PGresult.
		 */
		res = PQexec(fmstate->conn, sql);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			pgfdw_report_error(ERROR, res, true, sql);
		PQclear(res);
		fmstate->p_name = NULL;
	}

	/* Release remote connection */
	ReleaseConnection(fmstate->conn);
	fmstate->conn = NULL;
}

/*
 * postgresExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void
postgresExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	List	   *fdw_private;
	char	   *sql;

	if (es->verbose)
	{
		fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;
		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
		ExplainPropertyText("Remote SQL", sql, es);
	}
}

/*
 * postgresExplainForeignModify
 *		Produce extra output for EXPLAIN of a ModifyTable on a foreign table
 */
static void
postgresExplainForeignModify(ModifyTableState *mtstate,
							 ResultRelInfo *rinfo,
							 List *fdw_private,
							 int subplan_index,
							 ExplainState *es)
{
	if (es->verbose)
	{
		char	   *sql = strVal(list_nth(fdw_private,
										  FdwModifyPrivateUpdateSql));

		ExplainPropertyText("Remote SQL", sql, es);
	}
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
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	int			numParams = fsstate->numParams;
	Oid		   *types = fsstate->param_types;
	const char **values = fsstate->param_values;
	PGconn	   *conn = fsstate->conn;
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
	if (numParams > 0 && !fsstate->extparams_done)
	{
		ParamListInfo params = node->ss.ps.state->es_param_list_info;
		int			nestlevel;
		List	   *param_numbers;
		ListCell   *lc;

		nestlevel = set_transmission_modes();

		param_numbers = (List *)
			list_nth(fsstate->fdw_private, FdwScanPrivateExternParamIds);
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
			 * except that there may be unused holes in the array, which will
			 * have to be filled with something or the remote server will
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

		reset_transmission_modes(nestlevel);

		fsstate->extparams_done = true;
	}

	/* Construct the DECLARE CURSOR command */
	sql = strVal(list_nth(fsstate->fdw_private, FdwScanPrivateSelectSql));
	initStringInfo(&buf);
	appendStringInfo(&buf, "DECLARE c%u CURSOR FOR\n%s",
					 fsstate->cursor_number, sql);

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
	fsstate->cursor_exists = true;
	fsstate->tuples = NULL;
	fsstate->num_tuples = 0;
	fsstate->next_tuple = 0;
	fsstate->fetch_ct_2 = 0;
	fsstate->eof_reached = false;

	/* Clean up */
	pfree(buf.data);
}

/*
 * Fetch some more rows from the node's cursor.
 */
static void
fetch_more_data(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	PGresult   *volatile res = NULL;
	MemoryContext oldcontext;

	/*
	 * We'll store the tuples in the batch_cxt.  First, flush the previous
	 * batch.
	 */
	fsstate->tuples = NULL;
	MemoryContextReset(fsstate->batch_cxt);
	oldcontext = MemoryContextSwitchTo(fsstate->batch_cxt);

	/* PGresult must be released before leaving this function. */
	PG_TRY();
	{
		PGconn	   *conn = fsstate->conn;
		char		sql[64];
		int			fetch_size;
		int			numrows;
		int			i;

		/* The fetch size is arbitrary, but shouldn't be enormous. */
		fetch_size = 100;

		snprintf(sql, sizeof(sql), "FETCH %d FROM c%u",
				 fetch_size, fsstate->cursor_number);

		res = PQexec(conn, sql);
		/* On error, report the original query, not the FETCH. */
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, false,
							   strVal(list_nth(fsstate->fdw_private,
											   FdwScanPrivateSelectSql)));

		/* Convert the data into HeapTuples */
		numrows = PQntuples(res);
		fsstate->tuples = (HeapTuple *) palloc0(numrows * sizeof(HeapTuple));
		fsstate->num_tuples = numrows;
		fsstate->next_tuple = 0;

		for (i = 0; i < numrows; i++)
		{
			fsstate->tuples[i] =
				make_tuple_from_result_row(res, i,
										   fsstate->rel,
										   fsstate->attinmeta,
										   fsstate->temp_cxt);
		}

		/* Update fetch_ct_2 */
		if (fsstate->fetch_ct_2 < 2)
			fsstate->fetch_ct_2++;

		/* Must be EOF if we didn't get as many tuples as we asked for. */
		fsstate->eof_reached = (numrows < fetch_size);

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
 * Force assorted GUC parameters to settings that ensure that we'll output
 * data values in a form that is unambiguous to the remote server.
 *
 * This is rather expensive and annoying to do once per row, but there's
 * little choice if we want to be sure values are transmitted accurately;
 * we can't leave the settings in place between rows for fear of affecting
 * user-visible computations.
 *
 * We use the equivalent of a function SET option to allow the settings to
 * persist only until the caller calls reset_transmission_modes().	If an
 * error is thrown in between, guc.c will take care of undoing the settings.
 *
 * The return value is the nestlevel that must be passed to
 * reset_transmission_modes() to undo things.
 */
int
set_transmission_modes(void)
{
	int			nestlevel = NewGUCNestLevel();

	/*
	 * The values set here should match what pg_dump does.	See also
	 * configure_remote_session in connection.c.
	 */
	if (DateStyle != USE_ISO_DATES)
		(void) set_config_option("datestyle", "ISO",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0);
	if (IntervalStyle != INTSTYLE_POSTGRES)
		(void) set_config_option("intervalstyle", "postgres",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0);
	if (extra_float_digits < 3)
		(void) set_config_option("extra_float_digits", "3",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0);

	return nestlevel;
}

/*
 * Undo the effects of set_transmission_modes().
 */
void
reset_transmission_modes(int nestlevel)
{
	AtEOXact_GUC(true, nestlevel);
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
 * prepare_foreign_modify
 *		Establish a prepared statement for execution of INSERT/UPDATE/DELETE
 */
static void
prepare_foreign_modify(PgFdwModifyState *fmstate)
{
	char		prep_name[NAMEDATALEN];
	char	   *p_name;
	PGresult   *res;

	/* Construct name we'll use for the prepared statement. */
	snprintf(prep_name, sizeof(prep_name), "pgsql_fdw_prep_%u",
			 GetPrepStmtNumber(fmstate->conn));
	p_name = pstrdup(prep_name);

	/*
	 * We intentionally do not specify parameter types here, but leave the
	 * remote server to derive them by default.  This avoids possible problems
	 * with the remote server using different type OIDs than we do.  All of
	 * the prepared statements we use in this module are simple enough that
	 * the remote server will make the right choices.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQprepare(fmstate->conn,
					p_name,
					fmstate->query,
					0,
					NULL);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, true, fmstate->query);
	PQclear(res);

	/* This action shows that the prepare has been done. */
	fmstate->p_name = p_name;
}

/*
 * convert_prep_stmt_params
 *		Create array of text strings representing parameter values
 *
 * tupleid is ctid to send, or NULL if none
 * slot is slot to get remaining parameters from, or NULL if none
 *
 * Data is constructed in temp_cxt; caller should reset that after use.
 */
static const char **
convert_prep_stmt_params(PgFdwModifyState *fmstate,
						 ItemPointer tupleid,
						 TupleTableSlot *slot)
{
	const char **p_values;
	int			pindex = 0;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);

	p_values = (const char **) palloc(sizeof(char *) * fmstate->p_nums);

	/* 1st parameter should be ctid, if it's in use */
	if (tupleid != NULL)
	{
		/* don't need set_transmission_modes for TID output */
		p_values[pindex] = OutputFunctionCall(&fmstate->p_flinfo[pindex],
											  PointerGetDatum(tupleid));
		pindex++;
	}

	/* get following parameters from slot */
	if (slot != NULL && fmstate->target_attrs != NIL)
	{
		int			nestlevel;
		ListCell   *lc;

		nestlevel = set_transmission_modes();

		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Datum		value;
			bool		isnull;

			value = slot_getattr(slot, attnum, &isnull);
			if (isnull)
				p_values[pindex] = NULL;
			else
				p_values[pindex] = OutputFunctionCall(&fmstate->p_flinfo[pindex],
													  value);
			pindex++;
		}

		reset_transmission_modes(nestlevel);
	}

	Assert(pindex == fmstate->p_nums);

	MemoryContextSwitchTo(oldcontext);

	return p_values;
}

/*
 * store_returning_result
 *		Store the result of a RETURNING clause
 *
 * On error, be sure to release the PGresult on the way out.  Callers do not
 * have PG_TRY blocks to ensure this happens.
 */
static void
store_returning_result(PgFdwModifyState *fmstate,
					   TupleTableSlot *slot, PGresult *res)
{
	/* PGresult must be released before leaving this function. */
	PG_TRY();
	{
		HeapTuple	newtup;

		newtup = make_tuple_from_result_row(res, 0,
											fmstate->rel,
											fmstate->attinmeta,
											fmstate->temp_cxt);
		/* tuple will be deleted when it is cleared from the slot */
		ExecStoreTuple(newtup, slot, InvalidBuffer, true);
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
	 * Now we have to get the number of pages.	It's annoying that the ANALYZE
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
	conn = GetConnection(server, user, false);

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
	conn = GetConnection(server, user, false);

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
	ItemPointer ctid = NULL;
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

	/*
	 * Convert ctid if present.  XXX we could stand to have a cleaner way of
	 * detecting whether ctid is included in the result.
	 */
	if (j < PQnfields(res))
	{
		char	   *valstr;
		Datum		datum;

		valstr = PQgetvalue(res, row, j);
		datum = DirectFunctionCall1(tidin, CStringGetDatum(valstr));
		ctid = (ItemPointer) DatumGetPointer(datum);
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

	if (ctid)
		tuple->t_self = *ctid;

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
