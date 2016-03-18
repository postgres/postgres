/*-------------------------------------------------------------------------
 *
 * postgres_fdw.c
 *		  Foreign-data wrapper for remote PostgreSQL servers
 *
 * Portions Copyright (c) 2012-2016, PostgreSQL Global Development Group
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
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"

PG_MODULE_MAGIC;

/* Default CPU cost to start up a foreign query. */
#define DEFAULT_FDW_STARTUP_COST	100.0

/* Default CPU cost to process 1 row (above and beyond cpu_tuple_cost). */
#define DEFAULT_FDW_TUPLE_COST		0.01

/* If no remote estimates, assume a sort costs 20% extra */
#define DEFAULT_FDW_SORT_MULTIPLIER 1.2

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the SELECT statement:
 *		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
 */
enum FdwScanPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwScanPrivateSelectSql,
	/* List of restriction clauses that can be executed remotely */
	FdwScanPrivateRemoteConds,
	/* Integer list of attribute numbers retrieved by the SELECT */
	FdwScanPrivateRetrievedAttrs,
	/* Integer representing the desired fetch_size */
	FdwScanPrivateFetchSize,
	/* Oid of user mapping to be used while connecting to the foreign server */
	FdwScanPrivateUserMappingOid,

	/*
	 * String describing join i.e. names of relations being joined and types
	 * of join, added when the scan is join
	 */
	FdwScanPrivateRelations
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a postgres_fdw foreign table.  We store:
 *
 * 1) INSERT/UPDATE/DELETE statement text to be sent to the remote server
 * 2) Integer list of target attribute numbers for INSERT/UPDATE
 *	  (NIL for a DELETE)
 * 3) Boolean flag showing if the remote query has a RETURNING clause
 * 4) Integer list of attribute numbers retrieved by RETURNING, if any
 */
enum FdwModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwModifyPrivateUpdateSql,
	/* Integer list of target attribute numbers for INSERT/UPDATE */
	FdwModifyPrivateTargetAttnums,
	/* has-returning flag (as an integer Value node) */
	FdwModifyPrivateHasReturning,
	/* Integer list of attribute numbers retrieved by RETURNING */
	FdwModifyPrivateRetrievedAttrs
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ForeignScan node that modifies a foreign table directly.  We store:
 *
 * 1) UPDATE/DELETE statement text to be sent to the remote server
 * 2) Boolean flag showing if the remote query has a RETURNING clause
 * 3) Integer list of attribute numbers retrieved by RETURNING, if any
 * 4) Boolean flag showing if we set the command es_processed
 */
enum FdwDirectModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwDirectModifyPrivateUpdateSql,
	/* has-returning flag (as an integer Value node) */
	FdwDirectModifyPrivateHasReturning,
	/* Integer list of attribute numbers retrieved by RETURNING */
	FdwDirectModifyPrivateRetrievedAttrs,
	/* set-processed flag (as an integer Value node) */
	FdwDirectModifyPrivateSetProcessed
};

/*
 * Execution state of a foreign scan using postgres_fdw.
 */
typedef struct PgFdwScanState
{
	Relation	rel;			/* relcache entry for the foreign table. NULL
								 * for a foreign join scan. */
	TupleDesc	tupdesc;		/* tuple descriptor of scan */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* extracted fdw_private data */
	char	   *query;			/* text of SELECT command */
	List	   *retrieved_attrs;	/* list of retrieved attribute numbers */

	/* for remote query execution */
	PGconn	   *conn;			/* connection for the scan */
	unsigned int cursor_number; /* quasi-unique ID for my cursor */
	bool		cursor_exists;	/* have we created the cursor? */
	int			numParams;		/* number of parameters passed to query */
	FmgrInfo   *param_flinfo;	/* output conversion functions for them */
	List	   *param_exprs;	/* executable expressions for param values */
	const char **param_values;	/* textual values of query parameters */

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

	int			fetch_size;		/* number of tuples per fetch */
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
	List	   *retrieved_attrs;	/* attr numbers retrieved by RETURNING */

	/* info about parameters for prepared statement */
	AttrNumber	ctidAttno;		/* attnum of input resjunk ctid column */
	int			p_nums;			/* number of parameters to transmit */
	FmgrInfo   *p_flinfo;		/* output conversion functions for them */

	/* working memory context */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} PgFdwModifyState;

/*
 * Execution state of a foreign scan that modifies a foreign table directly.
 */
typedef struct PgFdwDirectModifyState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* extracted fdw_private data */
	char	   *query;			/* text of UPDATE/DELETE command */
	bool		has_returning;	/* is there a RETURNING clause? */
	List	   *retrieved_attrs;	/* attr numbers retrieved by RETURNING */
	bool		set_processed;	/* do we set the command es_processed? */

	/* for remote query execution */
	PGconn	   *conn;			/* connection for the update */
	int			numParams;		/* number of parameters passed to query */
	FmgrInfo   *param_flinfo;	/* output conversion functions for them */
	List	   *param_exprs;	/* executable expressions for param values */
	const char **param_values;	/* textual values of query parameters */

	/* for storing result tuples */
	PGresult   *result;			/* result for query */
	int			num_tuples;		/* # of result tuples */
	int			next_tuple;		/* index of next one to return */

	/* working memory context */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} PgFdwDirectModifyState;

/*
 * Workspace for analyzing a foreign table.
 */
typedef struct PgFdwAnalyzeState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */
	List	   *retrieved_attrs;	/* attr numbers retrieved by query */

	/* collected sample rows */
	HeapTuple  *rows;			/* array of size targrows */
	int			targrows;		/* target # of sample rows */
	int			numrows;		/* # of sample rows collected */

	/* for random sampling */
	double		samplerows;		/* # of rows fetched */
	double		rowstoskip;		/* # of rows to skip before next sample */
	ReservoirStateData rstate;	/* state for reservoir sampling */

	/* working memory contexts */
	MemoryContext anl_cxt;		/* context for per-analyze lifespan data */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */
} PgFdwAnalyzeState;

/*
 * Identify the attribute where data conversion fails.
 */
typedef struct ConversionLocation
{
	Relation	rel;			/* foreign table's relcache entry. */
	AttrNumber	cur_attno;		/* attribute number being processed, or 0 */

	/*
	 * In case of foreign join push down, fdw_scan_tlist is used to identify
	 * the Var node corresponding to the error location and
	 * fsstate->ss.ps.state gives access to the RTEs of corresponding relation
	 * to get the relation name and attribute name.
	 */
	ForeignScanState *fsstate;
} ConversionLocation;

/* Callback argument for ec_member_matches_foreign */
typedef struct
{
	Expr	   *current;		/* current expr, or NULL if not yet found */
	List	   *already_used;	/* expressions already dealt with */
} ec_member_foreign_arg;

/*
 * SQL functions
 */
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
					   List *scan_clauses,
					   Plan *outer_plan);
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
static int	postgresIsForeignRelUpdatable(Relation rel);
static bool postgresPlanDirectModify(PlannerInfo *root,
									 ModifyTable *plan,
									 Index resultRelation,
									 int subplan_index);
static void postgresBeginDirectModify(ForeignScanState *node, int eflags);
static TupleTableSlot *postgresIterateDirectModify(ForeignScanState *node);
static void postgresEndDirectModify(ForeignScanState *node);
static void postgresExplainForeignScan(ForeignScanState *node,
						   ExplainState *es);
static void postgresExplainForeignModify(ModifyTableState *mtstate,
							 ResultRelInfo *rinfo,
							 List *fdw_private,
							 int subplan_index,
							 ExplainState *es);
static void postgresExplainDirectModify(ForeignScanState *node,
										ExplainState *es);
static bool postgresAnalyzeForeignTable(Relation relation,
							AcquireSampleRowsFunc *func,
							BlockNumber *totalpages);
static List *postgresImportForeignSchema(ImportForeignSchemaStmt *stmt,
							Oid serverOid);
static void postgresGetForeignJoinPaths(PlannerInfo *root,
							RelOptInfo *joinrel,
							RelOptInfo *outerrel,
							RelOptInfo *innerrel,
							JoinType jointype,
							JoinPathExtraData *extra);
static bool postgresRecheckForeignScan(ForeignScanState *node,
						   TupleTableSlot *slot);

/*
 * Helper functions
 */
static void estimate_path_cost_size(PlannerInfo *root,
						RelOptInfo *baserel,
						List *join_conds,
						List *pathkeys,
						double *p_rows, int *p_width,
						Cost *p_startup_cost, Cost *p_total_cost);
static void get_remote_estimate(const char *sql,
					PGconn *conn,
					double *rows,
					int *width,
					Cost *startup_cost,
					Cost *total_cost);
static bool ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
						  EquivalenceClass *ec, EquivalenceMember *em,
						  void *arg);
static void create_cursor(ForeignScanState *node);
static void fetch_more_data(ForeignScanState *node);
static void close_cursor(PGconn *conn, unsigned int cursor_number);
static void prepare_foreign_modify(PgFdwModifyState *fmstate);
static const char **convert_prep_stmt_params(PgFdwModifyState *fmstate,
						 ItemPointer tupleid,
						 TupleTableSlot *slot);
static void store_returning_result(PgFdwModifyState *fmstate,
					   TupleTableSlot *slot, PGresult *res);
static void execute_dml_stmt(ForeignScanState *node);
static TupleTableSlot *get_returning_data(ForeignScanState *node);
static void prepare_query_params(PlanState *node,
					 List *fdw_exprs,
					 int numParams,
					 FmgrInfo **param_flinfo,
					 List **param_exprs,
					 const char ***param_values);
static void process_query_params(ExprContext *econtext,
					 FmgrInfo *param_flinfo,
					 List *param_exprs,
					 const char **param_values);
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
						   List *retrieved_attrs,
						   ForeignScanState *fsstate,
						   MemoryContext temp_context);
static void conversion_error_callback(void *arg);
static bool foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel,
				JoinType jointype, RelOptInfo *outerrel, RelOptInfo *innerrel,
				JoinPathExtraData *extra);
static List *get_useful_pathkeys_for_relation(PlannerInfo *root,
								 RelOptInfo *rel);
static List *get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel);
static void add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
								Path *epq_path);


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
	routine->IsForeignRelUpdatable = postgresIsForeignRelUpdatable;
	routine->PlanDirectModify = postgresPlanDirectModify;
	routine->BeginDirectModify = postgresBeginDirectModify;
	routine->IterateDirectModify = postgresIterateDirectModify;
	routine->EndDirectModify = postgresEndDirectModify;

	/* Function for EvalPlanQual rechecks */
	routine->RecheckForeignScan = postgresRecheckForeignScan;
	/* Support functions for EXPLAIN */
	routine->ExplainForeignScan = postgresExplainForeignScan;
	routine->ExplainForeignModify = postgresExplainForeignModify;
	routine->ExplainDirectModify = postgresExplainDirectModify;

	/* Support functions for ANALYZE */
	routine->AnalyzeForeignTable = postgresAnalyzeForeignTable;

	/* Support functions for IMPORT FOREIGN SCHEMA */
	routine->ImportForeignSchema = postgresImportForeignSchema;

	/* Support functions for join push-down */
	routine->GetForeignJoinPaths = postgresGetForeignJoinPaths;

	PG_RETURN_POINTER(routine);
}

/*
 * postgresGetForeignRelSize
 *		Estimate # of rows and width of the result of the scan
 *
 * We should consider the effect of all baserestrictinfo clauses here, but
 * not any join clauses.
 */
static void
postgresGetForeignRelSize(PlannerInfo *root,
						  RelOptInfo *baserel,
						  Oid foreigntableid)
{
	PgFdwRelationInfo *fpinfo;
	ListCell   *lc;
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
	const char *namespace;
	const char *relname;
	const char *refname;

	/*
	 * We use PgFdwRelationInfo to pass various information to subsequent
	 * functions.
	 */
	fpinfo = (PgFdwRelationInfo *) palloc0(sizeof(PgFdwRelationInfo));
	baserel->fdw_private = (void *) fpinfo;

	/* Base foreign tables need to be push down always. */
	fpinfo->pushdown_safe = true;

	/* Look up foreign-table catalog info. */
	fpinfo->table = GetForeignTable(foreigntableid);
	fpinfo->server = GetForeignServer(fpinfo->table->serverid);

	/*
	 * Extract user-settable option values.  Note that per-table setting of
	 * use_remote_estimate overrides per-server setting.
	 */
	fpinfo->use_remote_estimate = false;
	fpinfo->fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
	fpinfo->fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;
	fpinfo->shippable_extensions = NIL;
	fpinfo->fetch_size = 100;

	foreach(lc, fpinfo->server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_estimate") == 0)
			fpinfo->use_remote_estimate = defGetBoolean(def);
		else if (strcmp(def->defname, "fdw_startup_cost") == 0)
			fpinfo->fdw_startup_cost = strtod(defGetString(def), NULL);
		else if (strcmp(def->defname, "fdw_tuple_cost") == 0)
			fpinfo->fdw_tuple_cost = strtod(defGetString(def), NULL);
		else if (strcmp(def->defname, "extensions") == 0)
			fpinfo->shippable_extensions =
				ExtractExtensionList(defGetString(def), false);
		else if (strcmp(def->defname, "fetch_size") == 0)
			fpinfo->fetch_size = strtol(defGetString(def), NULL, 10);
	}
	foreach(lc, fpinfo->table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_estimate") == 0)
			fpinfo->use_remote_estimate = defGetBoolean(def);
		else if (strcmp(def->defname, "fetch_size") == 0)
			fpinfo->fetch_size = strtol(defGetString(def), NULL, 10);
	}

	/*
	 * If the table or the server is configured to use remote estimates,
	 * identify which user to do remote access as during planning.  This
	 * should match what ExecCheckRTEPerms() does.  If we fail due to lack of
	 * permissions, the query would have failed at runtime anyway.
	 */
	if (fpinfo->use_remote_estimate)
	{
		Oid			userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

		fpinfo->user = GetUserMapping(userid, fpinfo->server->serverid);
	}
	else
		fpinfo->user = NULL;

	/*
	 * Identify which baserestrictinfo clauses can be sent to the remote
	 * server and which can't.
	 */
	classifyConditions(root, baserel, baserel->baserestrictinfo,
					   &fpinfo->remote_conds, &fpinfo->local_conds);

	/*
	 * Identify which attributes will need to be retrieved from the remote
	 * server.  These include all attrs needed for joins or final output, plus
	 * all attrs used in the local_conds.  (Note: if we end up using a
	 * parameterized scan, it's possible that some of the join clauses will be
	 * sent to the remote and thus we wouldn't really need to retrieve the
	 * columns used in them.  Doesn't seem worth detecting that case though.)
	 */
	fpinfo->attrs_used = NULL;
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
				   &fpinfo->attrs_used);
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &fpinfo->attrs_used);
	}

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path.  The best we can do for these
	 * conditions is to estimate selectivity on the basis of local statistics.
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 baserel->relid,
													 JOIN_INNER,
													 NULL);

	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/*
	 * Set cached relation costs to some negative value, so that we can detect
	 * when they are set to some sensible costs during one (usually the first)
	 * of the calls to estimate_path_cost_size().
	 */
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * If the table or the server is configured to use remote estimates,
	 * connect to the foreign server and execute EXPLAIN to estimate the
	 * number of rows selected by the restriction clauses, as well as the
	 * average row width.  Otherwise, estimate using whatever statistics we
	 * have locally, in a way similar to ordinary tables.
	 */
	if (fpinfo->use_remote_estimate)
	{
		/*
		 * Get cost/size estimates with help of remote server.  Save the
		 * values in fpinfo so we don't need to do it again to generate the
		 * basic foreign path.
		 */
		estimate_path_cost_size(root, baserel, NIL, NIL,
								&fpinfo->rows, &fpinfo->width,
								&fpinfo->startup_cost, &fpinfo->total_cost);

		/* Report estimated baserel size to planner. */
		baserel->rows = fpinfo->rows;
		baserel->reltarget->width = fpinfo->width;
	}
	else
	{
		/*
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
				(10 * BLCKSZ) / (baserel->reltarget->width +
								 MAXALIGN(SizeofHeapTupleHeader));
		}

		/* Estimate baserel size as best we can with local statistics. */
		set_baserel_size_estimates(root, baserel);

		/* Fill in basically-bogus cost estimates for use later. */
		estimate_path_cost_size(root, baserel, NIL, NIL,
								&fpinfo->rows, &fpinfo->width,
								&fpinfo->startup_cost, &fpinfo->total_cost);
	}

	/*
	 * Set the name of relation in fpinfo, while we are constructing it here.
	 * It will be used to build the string describing the join relation in
	 * EXPLAIN output. We can't know whether VERBOSE option is specified or
	 * not, so always schema-qualify the foreign table name.
	 */
	fpinfo->relation_name = makeStringInfo();
	namespace = get_namespace_name(get_rel_namespace(foreigntableid));
	relname = get_rel_name(foreigntableid);
	refname = rte->eref->aliasname;
	appendStringInfo(fpinfo->relation_name, "%s.%s",
					 quote_identifier(namespace),
					 quote_identifier(relname));
	if (*refname && strcmp(refname, relname) != 0)
		appendStringInfo(fpinfo->relation_name, " %s",
						 quote_identifier(rte->eref->aliasname));
}

/*
 * get_useful_ecs_for_relation
 *		Determine which EquivalenceClasses might be involved in useful
 *		orderings of this relation.
 *
 * This function is in some respects a mirror image of the core function
 * pathkeys_useful_for_merging: for a regular table, we know what indexes
 * we have and want to test whether any of them are useful.  For a foreign
 * table, we don't know what indexes are present on the remote side but
 * want to speculate about which ones we'd like to use if they existed.
 */
static List *
get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_eclass_list = NIL;
	ListCell   *lc;
	Relids		relids;

	/*
	 * First, consider whether any active EC is potentially useful for a merge
	 * join against this relation.
	 */
	if (rel->has_eclass_joins)
	{
		foreach(lc, root->eq_classes)
		{
			EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc);

			if (eclass_useful_for_merging(root, cur_ec, rel))
				useful_eclass_list = lappend(useful_eclass_list, cur_ec);
		}
	}

	/*
	 * Next, consider whether there are any non-EC derivable join clauses that
	 * are merge-joinable.  If the joininfo list is empty, we can exit
	 * quickly.
	 */
	if (rel->joininfo == NIL)
		return useful_eclass_list;

	/* If this is a child rel, we must use the topmost parent rel to search. */
	if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		relids = find_childrel_top_parent(root, rel)->relids;
	else
		relids = rel->relids;

	/* Check each join clause in turn. */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/* Consider only mergejoinable clauses */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;

		/* Make sure we've got canonical ECs. */
		update_mergeclause_eclasses(root, restrictinfo);

		/*
		 * restrictinfo->mergeopfamilies != NIL is sufficient to guarantee
		 * that left_ec and right_ec will be initialized, per comments in
		 * distribute_qual_to_rels, and rel->joininfo should only contain ECs
		 * where this relation appears on one side or the other.
		 */
		if (bms_is_subset(relids, restrictinfo->right_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
													 restrictinfo->right_ec);
		else
		{
			Assert(bms_is_subset(relids, restrictinfo->left_ec->ec_relids));
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
													  restrictinfo->left_ec);
		}
	}

	return useful_eclass_list;
}

/*
 * get_useful_pathkeys_for_relation
 *		Determine which orderings of a relation might be useful.
 *
 * Getting data in sorted order can be useful either because the requested
 * order matches the final output ordering for the overall query we're
 * planning, or because it enables an efficient merge join.  Here, we try
 * to figure out which pathkeys to consider.
 */
static List *
get_useful_pathkeys_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_pathkeys_list = NIL;
	List	   *useful_eclass_list;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) rel->fdw_private;
	EquivalenceClass *query_ec = NULL;
	ListCell   *lc;

	/*
	 * Pushing the query_pathkeys to the remote server is always worth
	 * considering, because it might let us avoid a local sort.
	 */
	if (root->query_pathkeys)
	{
		bool		query_pathkeys_ok = true;

		foreach(lc, root->query_pathkeys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(lc);
			EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
			Expr	   *em_expr;

			/*
			 * The planner and executor don't have any clever strategy for
			 * taking data sorted by a prefix of the query's pathkeys and
			 * getting it to be sorted by all of those pathkeys. We'll just
			 * end up resorting the entire data set.  So, unless we can push
			 * down all of the query pathkeys, forget it.
			 *
			 * is_foreign_expr would detect volatile expressions as well, but
			 * checking ec_has_volatile here saves some cycles.
			 */
			if (pathkey_ec->ec_has_volatile ||
				!(em_expr = find_em_expr_for_rel(pathkey_ec, rel)) ||
				!is_foreign_expr(root, rel, em_expr))
			{
				query_pathkeys_ok = false;
				break;
			}
		}

		if (query_pathkeys_ok)
			useful_pathkeys_list = list_make1(list_copy(root->query_pathkeys));
	}

	/*
	 * Even if we're not using remote estimates, having the remote side do the
	 * sort generally won't be any worse than doing it locally, and it might
	 * be much better if the remote side can generate data in the right order
	 * without needing a sort at all.  However, what we're going to do next is
	 * try to generate pathkeys that seem promising for possible merge joins,
	 * and that's more speculative.  A wrong choice might hurt quite a bit, so
	 * bail out if we can't use remote estimates.
	 */
	if (!fpinfo->use_remote_estimate)
		return useful_pathkeys_list;

	/* Get the list of interesting EquivalenceClasses. */
	useful_eclass_list = get_useful_ecs_for_relation(root, rel);

	/* Extract unique EC for query, if any, so we don't consider it again. */
	if (list_length(root->query_pathkeys) == 1)
	{
		PathKey    *query_pathkey = linitial(root->query_pathkeys);

		query_ec = query_pathkey->pk_eclass;
	}

	/*
	 * As a heuristic, the only pathkeys we consider here are those of length
	 * one.  It's surely possible to consider more, but since each one we
	 * choose to consider will generate a round-trip to the remote side, we
	 * need to be a bit cautious here.  It would sure be nice to have a local
	 * cache of information about remote index definitions...
	 */
	foreach(lc, useful_eclass_list)
	{
		EquivalenceClass *cur_ec = lfirst(lc);
		Expr	   *em_expr;
		PathKey    *pathkey;

		/* If redundant with what we did above, skip it. */
		if (cur_ec == query_ec)
			continue;

		/* If no pushable expression for this rel, skip it. */
		em_expr = find_em_expr_for_rel(cur_ec, rel);
		if (em_expr == NULL || !is_foreign_expr(root, rel, em_expr))
			continue;

		/* Looks like we can generate a pathkey, so let's do it. */
		pathkey = make_canonical_pathkey(root, cur_ec,
										 linitial_oid(cur_ec->ec_opfamilies),
										 BTLessStrategyNumber,
										 false);
		useful_pathkeys_list = lappend(useful_pathkeys_list,
									   list_make1(pathkey));
	}

	return useful_pathkeys_list;
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
	List	   *ppi_list;
	ListCell   *lc;

	/*
	 * Create simplest ForeignScan path node and add it to baserel.  This path
	 * corresponds to SeqScan path of regular tables (though depending on what
	 * baserestrict conditions we were able to send to remote, there might
	 * actually be an indexscan happening there).  We already did all the work
	 * to estimate cost and size of this path.
	 */
	path = create_foreignscan_path(root, baserel,
								   NULL,		/* default pathtarget */
								   fpinfo->rows,
								   fpinfo->startup_cost,
								   fpinfo->total_cost,
								   NIL, /* no pathkeys */
								   NULL,		/* no outer rel either */
								   NULL,		/* no extra plan */
								   NIL);		/* no fdw_private list */
	add_path(baserel, (Path *) path);

	/* Add paths with pathkeys */
	add_paths_with_pathkeys_for_rel(root, baserel, NULL);

	/*
	 * If we're not using remote estimates, stop here.  We have no way to
	 * estimate whether any join clauses would be worth sending across, so
	 * don't bother building parameterized paths.
	 */
	if (!fpinfo->use_remote_estimate)
		return;

	/*
	 * Thumb through all join clauses for the rel to identify which outer
	 * relations could supply one or more safe-to-send-to-remote join clauses.
	 * We'll build a parameterized path for each such outer relation.
	 *
	 * It's convenient to manage this by representing each candidate outer
	 * relation by the ParamPathInfo node for it.  We can then use the
	 * ppi_clauses list in the ParamPathInfo node directly as a list of the
	 * interesting join clauses for that rel.  This takes care of the
	 * possibility that there are multiple safe join clauses for such a rel,
	 * and also ensures that we account for unsafe join clauses that we'll
	 * still have to enforce locally (since the parameterized-path machinery
	 * insists that we handle all movable clauses).
	 */
	ppi_list = NIL;
	foreach(lc, baserel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Relids		required_outer;
		ParamPathInfo *param_info;

		/* Check if clause can be moved to this rel */
		if (!join_clause_is_movable_to(rinfo, baserel))
			continue;

		/* See if it is safe to send to remote */
		if (!is_foreign_expr(root, baserel, rinfo->clause))
			continue;

		/* Calculate required outer rels for the resulting path */
		required_outer = bms_union(rinfo->clause_relids,
								   baserel->lateral_relids);
		/* We do not want the foreign rel itself listed in required_outer */
		required_outer = bms_del_member(required_outer, baserel->relid);

		/*
		 * required_outer probably can't be empty here, but if it were, we
		 * couldn't make a parameterized path.
		 */
		if (bms_is_empty(required_outer))
			continue;

		/* Get the ParamPathInfo */
		param_info = get_baserel_parampathinfo(root, baserel,
											   required_outer);
		Assert(param_info != NULL);

		/*
		 * Add it to list unless we already have it.  Testing pointer equality
		 * is OK since get_baserel_parampathinfo won't make duplicates.
		 */
		ppi_list = list_append_unique_ptr(ppi_list, param_info);
	}

	/*
	 * The above scan examined only "generic" join clauses, not those that
	 * were absorbed into EquivalenceClauses.  See if we can make anything out
	 * of EquivalenceClauses.
	 */
	if (baserel->has_eclass_joins)
	{
		/*
		 * We repeatedly scan the eclass list looking for column references
		 * (or expressions) belonging to the foreign rel.  Each time we find
		 * one, we generate a list of equivalence joinclauses for it, and then
		 * see if any are safe to send to the remote.  Repeat till there are
		 * no more candidate EC members.
		 */
		ec_member_foreign_arg arg;

		arg.already_used = NIL;
		for (;;)
		{
			List	   *clauses;

			/* Make clauses, skipping any that join to lateral_referencers */
			arg.current = NULL;
			clauses = generate_implied_equalities_for_column(root,
															 baserel,
												   ec_member_matches_foreign,
															 (void *) &arg,
											   baserel->lateral_referencers);

			/* Done if there are no more expressions in the foreign rel */
			if (arg.current == NULL)
			{
				Assert(clauses == NIL);
				break;
			}

			/* Scan the extracted join clauses */
			foreach(lc, clauses)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
				Relids		required_outer;
				ParamPathInfo *param_info;

				/* Check if clause can be moved to this rel */
				if (!join_clause_is_movable_to(rinfo, baserel))
					continue;

				/* See if it is safe to send to remote */
				if (!is_foreign_expr(root, baserel, rinfo->clause))
					continue;

				/* Calculate required outer rels for the resulting path */
				required_outer = bms_union(rinfo->clause_relids,
										   baserel->lateral_relids);
				required_outer = bms_del_member(required_outer, baserel->relid);
				if (bms_is_empty(required_outer))
					continue;

				/* Get the ParamPathInfo */
				param_info = get_baserel_parampathinfo(root, baserel,
													   required_outer);
				Assert(param_info != NULL);

				/* Add it to list unless we already have it */
				ppi_list = list_append_unique_ptr(ppi_list, param_info);
			}

			/* Try again, now ignoring the expression we found this time */
			arg.already_used = lappend(arg.already_used, arg.current);
		}
	}

	/*
	 * Now build a path for each useful outer relation.
	 */
	foreach(lc, ppi_list)
	{
		ParamPathInfo *param_info = (ParamPathInfo *) lfirst(lc);
		double		rows;
		int			width;
		Cost		startup_cost;
		Cost		total_cost;

		/* Get a cost estimate from the remote */
		estimate_path_cost_size(root, baserel,
								param_info->ppi_clauses, NIL,
								&rows, &width,
								&startup_cost, &total_cost);

		/*
		 * ppi_rows currently won't get looked at by anything, but still we
		 * may as well ensure that it matches our idea of the rowcount.
		 */
		param_info->ppi_rows = rows;

		/* Make the path */
		path = create_foreignscan_path(root, baserel,
									   NULL,	/* default pathtarget */
									   rows,
									   startup_cost,
									   total_cost,
									   NIL,		/* no pathkeys */
									   param_info->ppi_req_outer,
									   NULL,
									   NIL);	/* no fdw_private list */
		add_path(baserel, (Path *) path);
	}
}

/*
 * postgresGetForeignPlan
 *		Create ForeignScan plan node which implements selected best path
 */
static ForeignScan *
postgresGetForeignPlan(PlannerInfo *root,
					   RelOptInfo *foreignrel,
					   Oid foreigntableid,
					   ForeignPath *best_path,
					   List *tlist,
					   List *scan_clauses,
					   Plan *outer_plan)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;
	Index		scan_relid;
	List	   *fdw_private;
	List	   *remote_conds = NIL;
	List	   *remote_exprs = NIL;
	List	   *local_exprs = NIL;
	List	   *params_list = NIL;
	List	   *retrieved_attrs;
	StringInfoData sql;
	ListCell   *lc;
	List	   *fdw_scan_tlist = NIL;

	/*
	 * For base relations, set scan_relid as the relid of the relation. For
	 * other kinds of relations set it to 0.
	 */
	if (foreignrel->reloptkind == RELOPT_BASEREL ||
		foreignrel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		scan_relid = foreignrel->relid;
	else
	{
		scan_relid = 0;

		/*
		 * create_scan_plan() and create_foreignscan_plan() pass
		 * rel->baserestrictinfo + parameterization clauses through
		 * scan_clauses. For a join rel->baserestrictinfo is NIL and we are
		 * not considering parameterization right now, so there should be no
		 * scan_clauses for a joinrel.
		 */
		Assert(!scan_clauses);
	}

	/*
	 * Separate the scan_clauses into those that can be executed remotely and
	 * those that can't.  baserestrictinfo clauses that were previously
	 * determined to be safe or unsafe by classifyConditions are shown in
	 * fpinfo->remote_conds and fpinfo->local_conds.  Anything else in the
	 * scan_clauses list will be a join clause, which we have to check for
	 * remote-safety.
	 *
	 * Note: the join clauses we see here should be the exact same ones
	 * previously examined by postgresGetForeignPaths.  Possibly it'd be worth
	 * passing forward the classification work done then, rather than
	 * repeating it here.
	 *
	 * This code must match "extract_actual_clauses(scan_clauses, false)"
	 * except for the additional decision about remote versus local execution.
	 * Note however that we don't strip the RestrictInfo nodes from the
	 * remote_conds list, since appendWhereClause expects a list of
	 * RestrictInfos.
	 */
	foreach(lc, scan_clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(IsA(rinfo, RestrictInfo));

		/* Ignore any pseudoconstants, they're dealt with elsewhere */
		if (rinfo->pseudoconstant)
			continue;

		if (list_member_ptr(fpinfo->remote_conds, rinfo))
		{
			remote_conds = lappend(remote_conds, rinfo);
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		}
		else if (list_member_ptr(fpinfo->local_conds, rinfo))
			local_exprs = lappend(local_exprs, rinfo->clause);
		else if (is_foreign_expr(root, foreignrel, rinfo->clause))
		{
			remote_conds = lappend(remote_conds, rinfo);
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		}
		else
			local_exprs = lappend(local_exprs, rinfo->clause);
	}

	if (foreignrel->reloptkind == RELOPT_JOINREL)
	{
		/* For a join relation, get the conditions from fdw_private structure */
		remote_conds = fpinfo->remote_conds;
		local_exprs = fpinfo->local_conds;

		/* Build the list of columns to be fetched from the foreign server. */
		fdw_scan_tlist = build_tlist_to_deparse(foreignrel);

		/*
		 * Ensure that the outer plan produces a tuple whose descriptor
		 * matches our scan tuple slot. This is safe because all scans and
		 * joins support projection, so we never need to insert a Result node.
		 * Also, remove the local conditions from outer plan's quals, lest
		 * they will be evaluated twice, once by the local plan and once by
		 * the scan.
		 */
		if (outer_plan)
		{
			ListCell   *lc;

			outer_plan->targetlist = fdw_scan_tlist;

			foreach(lc, local_exprs)
			{
				Join	   *join_plan = (Join *) outer_plan;
				Node	   *qual = lfirst(lc);

				outer_plan->qual = list_delete(outer_plan->qual, qual);

				/*
				 * For an inner join the local conditions of foreign scan plan
				 * can be part of the joinquals as well.
				 */
				if (join_plan->jointype == JOIN_INNER)
					join_plan->joinqual = list_delete(join_plan->joinqual,
													  qual);
			}
		}
	}

	/*
	 * Build the query string to be sent for execution, and identify
	 * expressions to be sent as parameters.
	 */
	initStringInfo(&sql);
	deparseSelectStmtForRel(&sql, root, foreignrel, fdw_scan_tlist,
							remote_conds, best_path->path.pathkeys,
							&retrieved_attrs, &params_list);

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match order in enum FdwScanPrivateIndex.
	 */
	fdw_private = list_make5(makeString(sql.data),
							 remote_conds,
							 retrieved_attrs,
							 makeInteger(fpinfo->fetch_size),
							 makeInteger(foreignrel->umid));
	if (foreignrel->reloptkind == RELOPT_JOINREL)
		fdw_private = lappend(fdw_private,
							  makeString(fpinfo->relation_name->data));

	/*
	 * Create the ForeignScan node for the given relation.
	 *
	 * Note that the remote parameter expressions are stored in the fdw_exprs
	 * field of the finished plan node; we can't keep them in private state
	 * because then they wouldn't be subject to later planner processing.
	 */
	return make_foreignscan(tlist,
							local_exprs,
							scan_relid,
							params_list,
							fdw_private,
							fdw_scan_tlist,
							remote_exprs,
							outer_plan);
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
	UserMapping *user;
	int			numParams;

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
	 * Obtain the foreign server where to connect and user mapping to use for
	 * connection. For base relations we obtain this information from
	 * catalogs. For join relations, this information is frozen at the time of
	 * planning to ensure that the join is safe to pushdown. In case the
	 * information goes stale between planning and execution, plan will be
	 * invalidated and replanned.
	 */
	if (fsplan->scan.scanrelid > 0)
	{
		ForeignTable *table;

		/*
		 * Identify which user to do the remote access as.  This should match
		 * what ExecCheckRTEPerms() does.
		 */
		RangeTblEntry *rte = rt_fetch(fsplan->scan.scanrelid, estate->es_range_table);
		Oid			userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

		fsstate->rel = node->ss.ss_currentRelation;
		table = GetForeignTable(RelationGetRelid(fsstate->rel));

		user = GetUserMapping(userid, table->serverid);
	}
	else
	{
		Oid			umid = intVal(list_nth(fsplan->fdw_private, FdwScanPrivateUserMappingOid));

		user = GetUserMappingById(umid);
		Assert(fsplan->fs_server == user->serverid);
	}

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	fsstate->conn = GetConnection(user, false);

	/* Assign a unique ID for my cursor */
	fsstate->cursor_number = GetCursorNumber(fsstate->conn);
	fsstate->cursor_exists = false;

	/* Get private info created by planner functions. */
	fsstate->query = strVal(list_nth(fsplan->fdw_private,
									 FdwScanPrivateSelectSql));
	fsstate->retrieved_attrs = (List *) list_nth(fsplan->fdw_private,
											   FdwScanPrivateRetrievedAttrs);
	fsstate->fetch_size = intVal(list_nth(fsplan->fdw_private,
										  FdwScanPrivateFetchSize));

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

	/*
	 * Get info we'll need for converting data fetched from the foreign server
	 * into local representation and error reporting during that process.
	 */
	if (fsplan->scan.scanrelid > 0)
		fsstate->tupdesc = RelationGetDescr(fsstate->rel);
	else
		fsstate->tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	fsstate->attinmeta = TupleDescGetAttInMetadata(fsstate->tupdesc);

	/*
	 * Prepare for processing of parameters used in remote query, if any.
	 */
	numParams = list_length(fsplan->fdw_exprs);
	fsstate->numParams = numParams;
	if (numParams > 0)
		prepare_query_params((PlanState *) node,
							 fsplan->fdw_exprs,
							 numParams,
							 &fsstate->param_flinfo,
							 &fsstate->param_exprs,
							 &fsstate->param_values);
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

	/* If we haven't created the cursor yet, nothing to do. */
	if (!fsstate->cursor_exists)
		return;

	/*
	 * If any internal parameters affecting this node have changed, we'd
	 * better destroy and recreate the cursor.  Otherwise, rewinding it should
	 * be good enough.  If we've only fetched zero or one batch, we needn't
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
		pgfdw_report_error(ERROR, res, fsstate->conn, true, sql);
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
	List	   *retrieved_attrs = NIL;
	bool		doNothing = false;

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
		int			col;

		col = -1;
		while ((col = bms_next_member(rte->updatedCols, col)) >= 0)
		{
			/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
			AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

			if (attno <= InvalidAttrNumber)		/* shouldn't happen */
				elog(ERROR, "system-column update is not supported");
			targetAttrs = lappend_int(targetAttrs, attno);
		}
	}

	/*
	 * Extract the relevant RETURNING list if any.
	 */
	if (plan->returningLists)
		returningList = (List *) list_nth(plan->returningLists, subplan_index);

	/*
	 * ON CONFLICT DO UPDATE and DO NOTHING case with inference specification
	 * should have already been rejected in the optimizer, as presently there
	 * is no way to recognize an arbiter index on a foreign table.  Only DO
	 * NOTHING is supported without an inference specification.
	 */
	if (plan->onConflictAction == ONCONFLICT_NOTHING)
		doNothing = true;
	else if (plan->onConflictAction != ONCONFLICT_NONE)
		elog(ERROR, "unexpected ON CONFLICT specification: %d",
			 (int) plan->onConflictAction);

	/*
	 * Construct the SQL command string.
	 */
	switch (operation)
	{
		case CMD_INSERT:
			deparseInsertSql(&sql, root, resultRelation, rel,
							 targetAttrs, doNothing, returningList,
							 &retrieved_attrs);
			break;
		case CMD_UPDATE:
			deparseUpdateSql(&sql, root, resultRelation, rel,
							 targetAttrs, returningList,
							 &retrieved_attrs);
			break;
		case CMD_DELETE:
			deparseDeleteSql(&sql, root, resultRelation, rel,
							 returningList,
							 &retrieved_attrs);
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
	return list_make4(makeString(sql.data),
					  targetAttrs,
					  makeInteger((retrieved_attrs != NIL)),
					  retrieved_attrs);
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
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does.
	 */
	rte = rt_fetch(resultRelInfo->ri_RangeTableIndex, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	table = GetForeignTable(RelationGetRelid(rel));
	user = GetUserMapping(userid, table->serverid);

	/* Open connection; report that we'll create a prepared statement. */
	fmstate->conn = GetConnection(user, true);
	fmstate->p_name = NULL;		/* prepared statement not made yet */

	/* Deconstruct fdw_private data. */
	fmstate->query = strVal(list_nth(fdw_private,
									 FdwModifyPrivateUpdateSql));
	fmstate->target_attrs = (List *) list_nth(fdw_private,
											  FdwModifyPrivateTargetAttnums);
	fmstate->has_returning = intVal(list_nth(fdw_private,
											 FdwModifyPrivateHasReturning));
	fmstate->retrieved_attrs = (List *) list_nth(fdw_private,
											 FdwModifyPrivateRetrievedAttrs);

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
		pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);

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
		pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);

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
		pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);

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
			pgfdw_report_error(ERROR, res, fmstate->conn, true, sql);
		PQclear(res);
		fmstate->p_name = NULL;
	}

	/* Release remote connection */
	ReleaseConnection(fmstate->conn);
	fmstate->conn = NULL;
}

/*
 * postgresIsForeignRelUpdatable
 *		Determine whether a foreign table supports INSERT, UPDATE and/or
 *		DELETE.
 */
static int
postgresIsForeignRelUpdatable(Relation rel)
{
	bool		updatable;
	ForeignTable *table;
	ForeignServer *server;
	ListCell   *lc;

	/*
	 * By default, all postgres_fdw foreign tables are assumed updatable. This
	 * can be overridden by a per-server setting, which in turn can be
	 * overridden by a per-table setting.
	 */
	updatable = true;

	table = GetForeignTable(RelationGetRelid(rel));
	server = GetForeignServer(table->serverid);

	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "updatable") == 0)
			updatable = defGetBoolean(def);
	}
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "updatable") == 0)
			updatable = defGetBoolean(def);
	}

	/*
	 * Currently "updatable" means support for INSERT, UPDATE and DELETE.
	 */
	return updatable ?
		(1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE) : 0;
}

/*
 * postgresRecheckForeignScan
 *		Execute a local join execution plan for a foreign join
 */
static bool
postgresRecheckForeignScan(ForeignScanState *node, TupleTableSlot *slot)
{
	Index		scanrelid = ((Scan *) node->ss.ps.plan)->scanrelid;
	PlanState  *outerPlan = outerPlanState(node);
	TupleTableSlot *result;

	/* For base foreign relations, it suffices to set fdw_recheck_quals */
	if (scanrelid > 0)
		return true;

	Assert(outerPlan != NULL);

	/* Execute a local join execution plan */
	result = ExecProcNode(outerPlan);
	if (TupIsNull(result))
		return false;

	/* Store result in the given slot */
	ExecCopySlot(slot, result);

	return true;
}

/*
 * postgresPlanDirectModify
 *		Consider a direct foreign table modification
 *
 * Decide whether it is safe to modify a foreign table directly,  and if so,
 * rewrite subplan accordingly.
 */
static bool
postgresPlanDirectModify(PlannerInfo *root,
						 ModifyTable *plan,
						 Index resultRelation,
						 int subplan_index)
{
	CmdType		operation = plan->operation;
	Plan	   *subplan = (Plan *) list_nth(plan->plans, subplan_index);
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	StringInfoData sql;
	ForeignScan *fscan;
	List	   *targetAttrs = NIL;
	List	   *remote_conds;
	List	   *params_list = NIL;
	List	   *returningList = NIL;
	List	   *retrieved_attrs = NIL;

	/*
	 * Decide whether it is safe to modify a foreign table directly.
	 */

	/*
	 * The table modification must be an UPDATE or DELETE.
	 */
	if (operation != CMD_UPDATE && operation != CMD_DELETE)
		return false;

	/*
	 * It's unsafe to modify a foreign table directly if there are any local
	 * joins needed.
	 */
	if (!IsA(subplan, ForeignScan))
		return false;

	/*
	 * It's unsafe to modify a foreign table directly if there are any quals
	 * that should be evaluated locally.
	 */
	if (subplan->qual != NIL)
		return false;

	/*
	 * We can't handle an UPDATE or DELETE on a foreign join for now.
	 */
	fscan = (ForeignScan *) subplan;
	if (fscan->scan.scanrelid == 0)
		return false;

	/*
	 * It's unsafe to update a foreign table directly, if any expressions to
	 * assign to the target columns are unsafe to evaluate remotely.
	 */
	if (operation == CMD_UPDATE)
	{
		RelOptInfo *baserel = root->simple_rel_array[resultRelation];
		int			col;

		/*
		 * We transmit only columns that were explicitly targets of the UPDATE,
		 * so as to avoid unnecessary data transmission.
		 */
		col = -1;
		while ((col = bms_next_member(rte->updatedCols, col)) >= 0)
		{
			/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
			AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;
			TargetEntry *tle;

			if (attno <= InvalidAttrNumber)		/* shouldn't happen */
				elog(ERROR, "system-column update is not supported");

			tle = get_tle_by_resno(subplan->targetlist, attno);

			if (!is_foreign_expr(root, baserel, (Expr *) tle->expr))
				return false;

			targetAttrs = lappend_int(targetAttrs, attno);
		}
	}

	/*
	 * Ok, rewrite subplan so as to modify the foreign table directly.
	 */
	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	rel = heap_open(rte->relid, NoLock);

	/*
	 * Extract the baserestrictinfo clauses that can be evaluated remotely.
	 */
	remote_conds = (List *) list_nth(fscan->fdw_private,
									 FdwScanPrivateRemoteConds);

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
		case CMD_UPDATE:
			deparseDirectUpdateSql(&sql, root, resultRelation, rel,
								   ((Plan *) fscan)->targetlist,
								   targetAttrs,
								   remote_conds, &params_list,
								   returningList, &retrieved_attrs);
			break;
		case CMD_DELETE:
			deparseDirectDeleteSql(&sql, root, resultRelation, rel,
								   remote_conds, &params_list,
								   returningList, &retrieved_attrs);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	/*
	 * Update the operation info.
	 */
	fscan->operation = operation;

	/*
	 * Update the fdw_exprs list that will be available to the executor.
	 */
	fscan->fdw_exprs = params_list;

	/*
	 * Update the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwDirectModifyPrivateIndex, above.
	 */
	fscan->fdw_private = list_make4(makeString(sql.data),
									makeInteger((retrieved_attrs != NIL)),
									retrieved_attrs,
									makeInteger(plan->canSetTag));

	heap_close(rel, NoLock);
	return true;
}

/*
 * postgresBeginDirectModify
 *		Prepare a direct foreign table modification
 */
static void
postgresBeginDirectModify(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	PgFdwDirectModifyState *dmstate;
	RangeTblEntry *rte;
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	int			numParams;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * We'll save private state in node->fdw_state.
	 */
	dmstate = (PgFdwDirectModifyState *) palloc0(sizeof(PgFdwDirectModifyState));
	node->fdw_state = (void *) dmstate;

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does.
	 */
	rte = rt_fetch(fsplan->scan.scanrelid, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	dmstate->rel = node->ss.ss_currentRelation;
	table = GetForeignTable(RelationGetRelid(dmstate->rel));
	user = GetUserMapping(userid, table->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	dmstate->conn = GetConnection(user, false);

	/* Initialize state variable */
	dmstate->num_tuples = -1;		/* -1 means not set yet */

	/* Get private info created by planner functions. */
	dmstate->query = strVal(list_nth(fsplan->fdw_private,
									 FdwDirectModifyPrivateUpdateSql));
	dmstate->has_returning = intVal(list_nth(fsplan->fdw_private,
										 FdwDirectModifyPrivateHasReturning));
	dmstate->retrieved_attrs = (List *) list_nth(fsplan->fdw_private,
										FdwDirectModifyPrivateRetrievedAttrs);
	dmstate->set_processed = intVal(list_nth(fsplan->fdw_private,
										 FdwDirectModifyPrivateSetProcessed));

	/* Create context for per-tuple temp workspace. */
	dmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_MINSIZE,
											  ALLOCSET_SMALL_INITSIZE,
											  ALLOCSET_SMALL_MAXSIZE);

	/* Prepare for input conversion of RETURNING results. */
	if (dmstate->has_returning)
		dmstate->attinmeta = TupleDescGetAttInMetadata(RelationGetDescr(dmstate->rel));

	/*
	 * Prepare for processing of parameters used in remote query, if any.
	 */
	numParams = list_length(fsplan->fdw_exprs);
	dmstate->numParams = numParams;
	if (numParams > 0)
		prepare_query_params((PlanState *) node,
							 fsplan->fdw_exprs,
							 numParams,
							 &dmstate->param_flinfo,
							 &dmstate->param_exprs,
							 &dmstate->param_values);
}

/*
 * postgresIterateDirectModify
 *		Execute a direct foreign table modification
 */
static TupleTableSlot *
postgresIterateDirectModify(ForeignScanState *node)
{
	PgFdwDirectModifyState *dmstate = (PgFdwDirectModifyState *) node->fdw_state;
	EState	   *estate = node->ss.ps.state;
	ResultRelInfo *resultRelInfo = estate->es_result_relation_info;

	/*
	 * If this is the first call after Begin, execute the statement.
	 */
	if (dmstate->num_tuples == -1)
		execute_dml_stmt(node);

	/*
	 * If the local query doesn't specify RETURNING, just clear tuple slot.
	 */
	if (!resultRelInfo->ri_projectReturning)
	{
		TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
		Instrumentation *instr = node->ss.ps.instrument;

		Assert(!dmstate->has_returning);

		/* Increment the command es_processed count if necessary. */
		if (dmstate->set_processed)
			estate->es_processed += dmstate->num_tuples;

		/* Increment the tuple count for EXPLAIN ANALYZE if necessary. */
		if (instr)
			instr->tuplecount += dmstate->num_tuples;

		return ExecClearTuple(slot);
	}

	/*
	 * Get the next RETURNING tuple.
	 */
	return get_returning_data(node);
}

/*
 * postgresEndDirectModify
 *		Finish a direct foreign table modification
 */
static void
postgresEndDirectModify(ForeignScanState *node)
{
	PgFdwDirectModifyState *dmstate = (PgFdwDirectModifyState *) node->fdw_state;

	/* if dmstate is NULL, we are in EXPLAIN; nothing to do */
	if (dmstate == NULL)
		return;

	/* Release PGresult */
	if (dmstate->result)
		PQclear(dmstate->result);

	/* Release remote connection */
	ReleaseConnection(dmstate->conn);
	dmstate->conn = NULL;

	/* MemoryContext will be deleted automatically. */
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
	char	   *relations;

	fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;

	/*
	 * Add names of relation handled by the foreign scan when the scan is a
	 * join
	 */
	if (list_length(fdw_private) > FdwScanPrivateRelations)
	{
		relations = strVal(list_nth(fdw_private, FdwScanPrivateRelations));
		ExplainPropertyText("Relations", relations, es);
	}

	/*
	 * Add remote query, when VERBOSE option is specified.
	 */
	if (es->verbose)
	{
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
 * postgresExplainDirectModify
 *		Produce extra output for EXPLAIN of a ForeignScan that modifies a
 *		foreign table directly
 */
static void
postgresExplainDirectModify(ForeignScanState *node, ExplainState *es)
{
	List	   *fdw_private;
	char	   *sql;

	if (es->verbose)
	{
		fdw_private = ((ForeignScan *) node->ss.ps.plan)->fdw_private;
		sql = strVal(list_nth(fdw_private, FdwDirectModifyPrivateUpdateSql));
		ExplainPropertyText("Remote SQL", sql, es);
	}
}


/*
 * estimate_path_cost_size
 *		Get cost and size estimates for a foreign scan on given foreign relation
 *		either a base relation or a join between foreign relations.
 *
 * param_join_conds are the parameterization clauses with outer relations.
 * pathkeys specify the expected sort order if any for given path being costed.
 *
 * The function returns the cost and size estimates in p_row, p_width,
 * p_startup_cost and p_total_cost variables.
 */
static void
estimate_path_cost_size(PlannerInfo *root,
						RelOptInfo *foreignrel,
						List *param_join_conds,
						List *pathkeys,
						double *p_rows, int *p_width,
						Cost *p_startup_cost, Cost *p_total_cost)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;
	double		rows;
	double		retrieved_rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	Cost		cpu_per_tuple;

	/*
	 * If the table or the server is configured to use remote estimates,
	 * connect to the foreign server and execute EXPLAIN to estimate the
	 * number of rows selected by the restriction+join clauses.  Otherwise,
	 * estimate rows using whatever statistics we have locally, in a way
	 * similar to ordinary tables.
	 */
	if (fpinfo->use_remote_estimate)
	{
		List	   *remote_param_join_conds;
		List	   *local_param_join_conds;
		StringInfoData sql;
		PGconn	   *conn;
		Selectivity local_sel;
		QualCost	local_cost;
		List	   *fdw_scan_tlist = NIL;
		List	   *remote_conds;

		/* Required only to be passed to deparseSelectStmtForRel */
		List	   *retrieved_attrs;

		/*
		 * param_join_conds might contain both clauses that are safe to send
		 * across, and clauses that aren't.
		 */
		classifyConditions(root, foreignrel, param_join_conds,
						   &remote_param_join_conds, &local_param_join_conds);

		/* Build the list of columns to be fetched from the foreign server. */
		if (foreignrel->reloptkind == RELOPT_JOINREL)
			fdw_scan_tlist = build_tlist_to_deparse(foreignrel);
		else
			fdw_scan_tlist = NIL;

		/*
		 * The complete list of remote conditions includes everything from
		 * baserestrictinfo plus any extra join_conds relevant to this
		 * particular path.
		 */
		remote_conds = list_concat(list_copy(remote_param_join_conds),
								   fpinfo->remote_conds);

		/*
		 * Construct EXPLAIN query including the desired SELECT, FROM, and
		 * WHERE clauses. Params and other-relation Vars are replaced by dummy
		 * values, so don't request params_list.
		 */
		initStringInfo(&sql);
		appendStringInfoString(&sql, "EXPLAIN ");
		deparseSelectStmtForRel(&sql, root, foreignrel, fdw_scan_tlist,
								remote_conds, pathkeys, &retrieved_attrs,
								NULL);

		/* Get the remote estimate */
		conn = GetConnection(fpinfo->user, false);
		get_remote_estimate(sql.data, conn, &rows, &width,
							&startup_cost, &total_cost);
		ReleaseConnection(conn);

		retrieved_rows = rows;

		/* Factor in the selectivity of the locally-checked quals */
		local_sel = clauselist_selectivity(root,
										   local_param_join_conds,
										   foreignrel->relid,
										   JOIN_INNER,
										   NULL);
		local_sel *= fpinfo->local_conds_sel;

		rows = clamp_row_est(rows * local_sel);

		/* Add in the eval cost of the locally-checked quals */
		startup_cost += fpinfo->local_conds_cost.startup;
		total_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
		cost_qual_eval(&local_cost, local_param_join_conds, root);
		startup_cost += local_cost.startup;
		total_cost += local_cost.per_tuple * retrieved_rows;
	}
	else
	{
		Cost		run_cost = 0;

		/*
		 * We don't support join conditions in this mode (hence, no
		 * parameterized paths can be made).
		 */
		Assert(param_join_conds == NIL);

		/*
		 * Use rows/width estimates made by set_baserel_size_estimates() for
		 * base foreign relations and set_joinrel_size_estimates() for join
		 * between foreign relations.
		 */
		rows = foreignrel->rows;
		width = foreignrel->reltarget->width;

		/* Back into an estimate of the number of retrieved rows. */
		retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);

		/*
		 * We will come here again and again with different set of pathkeys
		 * that caller wants to cost. We don't need to calculate the cost of
		 * bare scan each time. Instead, use the costs if we have cached them
		 * already.
		 */
		if (fpinfo->rel_startup_cost > 0 && fpinfo->rel_total_cost > 0)
		{
			startup_cost = fpinfo->rel_startup_cost;
			run_cost = fpinfo->rel_total_cost - fpinfo->rel_startup_cost;
		}
		else if (foreignrel->reloptkind != RELOPT_JOINREL)
		{
			/* Clamp retrieved rows estimates to at most foreignrel->tuples. */
			retrieved_rows = Min(retrieved_rows, foreignrel->tuples);

			/*
			 * Cost as though this were a seqscan, which is pessimistic.  We
			 * effectively imagine the local_conds are being evaluated
			 * remotely, too.
			 */
			startup_cost = 0;
			run_cost = 0;
			run_cost += seq_page_cost * foreignrel->pages;

			startup_cost += foreignrel->baserestrictcost.startup;
			cpu_per_tuple = cpu_tuple_cost + foreignrel->baserestrictcost.per_tuple;
			run_cost += cpu_per_tuple * foreignrel->tuples;
		}
		else
		{
			PgFdwRelationInfo *fpinfo_i;
			PgFdwRelationInfo *fpinfo_o;
			QualCost	join_cost;
			QualCost	remote_conds_cost;
			double		nrows;

			/* For join we expect inner and outer relations set */
			Assert(fpinfo->innerrel && fpinfo->outerrel);

			fpinfo_i = (PgFdwRelationInfo *) fpinfo->innerrel->fdw_private;
			fpinfo_o = (PgFdwRelationInfo *) fpinfo->outerrel->fdw_private;

			/* Estimate of number of rows in cross product */
			nrows = fpinfo_i->rows * fpinfo_o->rows;
			/* Clamp retrieved rows estimate to at most size of cross product */
			retrieved_rows = Min(retrieved_rows, nrows);

			/*
			 * The cost of foreign join is estimated as cost of generating
			 * rows for the joining relations + cost for applying quals on the
			 * rows.
			 */

			/* Calculate the cost of clauses pushed down the foreign server */
			cost_qual_eval(&remote_conds_cost, fpinfo->remote_conds, root);
			/* Calculate the cost of applying join clauses */
			cost_qual_eval(&join_cost, fpinfo->joinclauses, root);

			/*
			 * Startup cost includes startup cost of joining relations and the
			 * startup cost for join and other clauses. We do not include the
			 * startup cost specific to join strategy (e.g. setting up hash
			 * tables) since we do not know what strategy the foreign server
			 * is going to use.
			 */
			startup_cost = fpinfo_i->rel_startup_cost + fpinfo_o->rel_startup_cost;
			startup_cost += join_cost.startup;
			startup_cost += remote_conds_cost.startup;
			startup_cost += fpinfo->local_conds_cost.startup;

			/*
			 * Run time cost includes:
			 *
			 * 1. Run time cost (total_cost - startup_cost) of relations being
			 * joined
			 *
			 * 2. Run time cost of applying join clauses on the cross product
			 * of the joining relations.
			 *
			 * 3. Run time cost of applying pushed down other clauses on the
			 * result of join
			 *
			 * 4. Run time cost of applying nonpushable other clauses locally
			 * on the result fetched from the foreign server.
			 */
			run_cost = fpinfo_i->rel_total_cost - fpinfo_i->rel_startup_cost;
			run_cost += fpinfo_o->rel_total_cost - fpinfo_o->rel_startup_cost;
			run_cost += nrows * join_cost.per_tuple;
			nrows = clamp_row_est(nrows * fpinfo->joinclause_sel);
			run_cost += nrows * remote_conds_cost.per_tuple;
			run_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
		}

		/*
		 * Without remote estimates, we have no real way to estimate the cost
		 * of generating sorted output.  It could be free if the query plan
		 * the remote side would have chosen generates properly-sorted output
		 * anyway, but in most cases it will cost something.  Estimate a value
		 * high enough that we won't pick the sorted path when the ordering
		 * isn't locally useful, but low enough that we'll err on the side of
		 * pushing down the ORDER BY clause when it's useful to do so.
		 */
		if (pathkeys != NIL)
		{
			startup_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
			run_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
		}

		total_cost = startup_cost + run_cost;
	}

	/*
	 * Cache the costs for scans without any pathkeys or parameterization
	 * before adding the costs for transferring data from the foreign server.
	 * These costs are useful for costing the join between this relation and
	 * another foreign relation or to calculate the costs of paths with
	 * pathkeys for this relation, when the costs can not be obtained from the
	 * foreign server. This function will be called at least once for every
	 * foreign relation without pathkeys and parameterization.
	 */
	if (pathkeys == NIL && param_join_conds == NIL)
	{
		fpinfo->rel_startup_cost = startup_cost;
		fpinfo->rel_total_cost = total_cost;
	}

	/*
	 * Add some additional cost factors to account for connection overhead
	 * (fdw_startup_cost), transferring data across the network
	 * (fdw_tuple_cost per retrieved row), and local manipulation of the data
	 * (cpu_tuple_cost per retrieved row).
	 */
	startup_cost += fpinfo->fdw_startup_cost;
	total_cost += fpinfo->fdw_startup_cost;
	total_cost += fpinfo->fdw_tuple_cost * retrieved_rows;
	total_cost += cpu_tuple_cost * retrieved_rows;

	/* Return results. */
	*p_rows = rows;
	*p_width = width;
	*p_startup_cost = startup_cost;
	*p_total_cost = total_cost;
}

/*
 * Estimate costs of executing a SQL statement remotely.
 * The given "sql" must be an EXPLAIN command.
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
		char	   *line;
		char	   *p;
		int			n;

		/*
		 * Execute EXPLAIN remotely.
		 */
		res = PQexec(conn, sql);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, sql);

		/*
		 * Extract cost numbers for topmost plan node.  Note we search for a
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
 * Detect whether we want to process an EquivalenceClass member.
 *
 * This is a callback for use by generate_implied_equalities_for_column.
 */
static bool
ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
						  EquivalenceClass *ec, EquivalenceMember *em,
						  void *arg)
{
	ec_member_foreign_arg *state = (ec_member_foreign_arg *) arg;
	Expr	   *expr = em->em_expr;

	/*
	 * If we've identified what we're processing in the current scan, we only
	 * want to match that expression.
	 */
	if (state->current != NULL)
		return equal(expr, state->current);

	/*
	 * Otherwise, ignore anything we've already processed.
	 */
	if (list_member(state->already_used, expr))
		return false;

	/* This is the new target to process. */
	state->current = expr;
	return true;
}

/*
 * Create cursor for node's query with current parameter values.
 */
static void
create_cursor(ForeignScanState *node)
{
	PgFdwScanState *fsstate = (PgFdwScanState *) node->fdw_state;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			numParams = fsstate->numParams;
	const char **values = fsstate->param_values;
	PGconn	   *conn = fsstate->conn;
	StringInfoData buf;
	PGresult   *res;

	/*
	 * Construct array of query parameter values in text format.  We do the
	 * conversions in the short-lived per-tuple context, so as not to cause a
	 * memory leak over repeated scans.
	 */
	if (numParams > 0)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);

		process_query_params(econtext,
							 fsstate->param_flinfo,
							 fsstate->param_exprs,
							 values);

		MemoryContextSwitchTo(oldcontext);
	}

	/* Construct the DECLARE CURSOR command */
	initStringInfo(&buf);
	appendStringInfo(&buf, "DECLARE c%u CURSOR FOR\n%s",
					 fsstate->cursor_number, fsstate->query);

	/*
	 * Notice that we pass NULL for paramTypes, thus forcing the remote server
	 * to infer types for all parameters.  Since we explicitly cast every
	 * parameter (see deparse.c), the "inference" is trivial and will produce
	 * the desired result.  This allows us to avoid assuming that the remote
	 * server has the same OIDs we do for the parameters' types.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = PQexecParams(conn, buf.data, numParams, NULL, values,
					   NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, fsstate->query);
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
		int			numrows;
		int			i;

		snprintf(sql, sizeof(sql), "FETCH %d FROM c%u",
				 fsstate->fetch_size, fsstate->cursor_number);

		res = PQexec(conn, sql);
		/* On error, report the original query, not the FETCH. */
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, fsstate->query);

		/* Convert the data into HeapTuples */
		numrows = PQntuples(res);
		fsstate->tuples = (HeapTuple *) palloc0(numrows * sizeof(HeapTuple));
		fsstate->num_tuples = numrows;
		fsstate->next_tuple = 0;

		for (i = 0; i < numrows; i++)
		{
			Assert(IsA(node->ss.ps.plan, ForeignScan));

			fsstate->tuples[i] =
				make_tuple_from_result_row(res, i,
										   fsstate->rel,
										   fsstate->attinmeta,
										   fsstate->retrieved_attrs,
										   node,
										   fsstate->temp_cxt);
		}

		/* Update fetch_ct_2 */
		if (fsstate->fetch_ct_2 < 2)
			fsstate->fetch_ct_2++;

		/* Must be EOF if we didn't get as many tuples as we asked for. */
		fsstate->eof_reached = (numrows < fsstate->fetch_size);

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
 * persist only until the caller calls reset_transmission_modes().  If an
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
	 * The values set here should match what pg_dump does.  See also
	 * configure_remote_session in connection.c.
	 */
	if (DateStyle != USE_ISO_DATES)
		(void) set_config_option("datestyle", "ISO",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (IntervalStyle != INTSTYLE_POSTGRES)
		(void) set_config_option("intervalstyle", "postgres",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (extra_float_digits < 3)
		(void) set_config_option("extra_float_digits", "3",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

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
		pgfdw_report_error(ERROR, res, conn, true, sql);
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
		pgfdw_report_error(ERROR, res, fmstate->conn, true, fmstate->query);
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
	PG_TRY();
	{
		HeapTuple	newtup;

		newtup = make_tuple_from_result_row(res, 0,
											fmstate->rel,
											fmstate->attinmeta,
											fmstate->retrieved_attrs,
											NULL,
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
 * Execute a direct UPDATE/DELETE statement.
 */
static void
execute_dml_stmt(ForeignScanState *node)
{
	PgFdwDirectModifyState *dmstate = (PgFdwDirectModifyState *) node->fdw_state;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	int			numParams = dmstate->numParams;
	const char **values = dmstate->param_values;

	/*
	 * Construct array of query parameter values in text format.
	 */
	if (numParams > 0)
		process_query_params(econtext,
							 dmstate->param_flinfo,
							 dmstate->param_exprs,
							 values);

	/*
	 * Notice that we pass NULL for paramTypes, thus forcing the remote server
	 * to infer types for all parameters.  Since we explicitly cast every
	 * parameter (see deparse.c), the "inference" is trivial and will produce
	 * the desired result.  This allows us to avoid assuming that the remote
	 * server has the same OIDs we do for the parameters' types.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	dmstate->result = PQexecParams(dmstate->conn, dmstate->query,
								   numParams, NULL, values, NULL, NULL, 0);
	if (PQresultStatus(dmstate->result) !=
		(dmstate->has_returning ? PGRES_TUPLES_OK : PGRES_COMMAND_OK))
		pgfdw_report_error(ERROR, dmstate->result, dmstate->conn, true,
						   dmstate->query);

	/* Get the number of rows affected. */
	if (dmstate->has_returning)
		dmstate->num_tuples = PQntuples(dmstate->result);
	else
		dmstate->num_tuples = atoi(PQcmdTuples(dmstate->result));
}

/*
 * Get the result of a RETURNING clause.
 */
static TupleTableSlot *
get_returning_data(ForeignScanState *node)
{
	PgFdwDirectModifyState *dmstate = (PgFdwDirectModifyState *) node->fdw_state;
	EState	   *estate = node->ss.ps.state;
	ResultRelInfo *resultRelInfo = estate->es_result_relation_info;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	Assert(resultRelInfo->ri_projectReturning);

	/* If we didn't get any tuples, must be end of data. */
	if (dmstate->next_tuple >= dmstate->num_tuples)
		return ExecClearTuple(slot);

	/* Increment the command es_processed count if necessary. */
	if (dmstate->set_processed)
		estate->es_processed += 1;

	/*
	 * Store a RETURNING tuple.  If has_returning is false, just emit a dummy
	 * tuple.  (has_returning is false when the local query is of the form
	 * "UPDATE/DELETE .. RETURNING 1" for example.)
	 */
	if (!dmstate->has_returning)
		ExecStoreAllNullTuple(slot);
	else
	{
		/*
		 * On error, be sure to release the PGresult on the way out.  Callers
		 * do not have PG_TRY blocks to ensure this happens.
		 */
		PG_TRY();
		{
			HeapTuple	newtup;

			newtup = make_tuple_from_result_row(dmstate->result,
												dmstate->next_tuple,
												dmstate->rel,
												dmstate->attinmeta,
												dmstate->retrieved_attrs,
												NULL,
												dmstate->temp_cxt);
			ExecStoreTuple(newtup, slot, InvalidBuffer, false);
		}
		PG_CATCH();
		{
			if (dmstate->result)
				PQclear(dmstate->result);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	dmstate->next_tuple++;

	/* Make slot available for evaluation of the local query RETURNING list. */
	resultRelInfo->ri_projectReturning->pi_exprContext->ecxt_scantuple = slot;

	return slot;
}

/*
 * Prepare for processing of parameters used in remote query.
 */
static void
prepare_query_params(PlanState *node,
					 List *fdw_exprs,
					 int numParams,
					 FmgrInfo **param_flinfo,
					 List **param_exprs,
					 const char ***param_values)
{
	int			i;
	ListCell   *lc;

	Assert(numParams > 0);

	/* Prepare for output conversion of parameters used in remote query. */
	*param_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * numParams);

	i = 0;
	foreach(lc, fdw_exprs)
	{
		Node	   *param_expr = (Node *) lfirst(lc);
		Oid			typefnoid;
		bool		isvarlena;

		getTypeOutputInfo(exprType(param_expr), &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &(*param_flinfo)[i]);
		i++;
	}

	/*
	 * Prepare remote-parameter expressions for evaluation.  (Note: in
	 * practice, we expect that all these expressions will be just Params, so
	 * we could possibly do something more efficient than using the full
	 * expression-eval machinery for this.  But probably there would be little
	 * benefit, and it'd require postgres_fdw to know more than is desirable
	 * about Param evaluation.)
	 */
	*param_exprs = (List *) ExecInitExpr((Expr *) fdw_exprs, node);

	/* Allocate buffer for text form of query parameters. */
	*param_values = (const char **) palloc0(numParams * sizeof(char *));
}

/*
 * Construct array of query parameter values in text format.
 */
static void
process_query_params(ExprContext *econtext,
					 FmgrInfo *param_flinfo,
					 List *param_exprs,
					 const char **param_values)
{
	int			nestlevel;
	int			i;
	ListCell   *lc;

	nestlevel = set_transmission_modes();

	i = 0;
	foreach(lc, param_exprs)
	{
		ExprState  *expr_state = (ExprState *) lfirst(lc);
		Datum		expr_value;
		bool		isNull;

		/* Evaluate the parameter expression */
		expr_value = ExecEvalExpr(expr_state, econtext, &isNull, NULL);

		/*
		 * Get string representation of each parameter value by invoking
		 * type-specific output function, unless the value is null.
		 */
		if (isNull)
			param_values[i] = NULL;
		else
			param_values[i] = OutputFunctionCall(&param_flinfo[i], expr_value);
			i++;
	}

	reset_transmission_modes(nestlevel);
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
	user = GetUserMapping(relation->rd_rel->relowner, table->serverid);
	conn = GetConnection(user, false);

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
			pgfdw_report_error(ERROR, res, conn, false, sql.data);

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
 * *totalrows.  Note that *totaldeadrows is always set to 0.
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
	reservoir_init_selection_state(&astate.rstate, targrows);

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
	user = GetUserMapping(relation->rd_rel->relowner, table->serverid);
	conn = GetConnection(user, false);

	/*
	 * Construct cursor that retrieves whole rows from remote.
	 */
	cursor_number = GetCursorNumber(conn);
	initStringInfo(&sql);
	appendStringInfo(&sql, "DECLARE c%u CURSOR FOR ", cursor_number);
	deparseAnalyzeSql(&sql, relation, &astate.retrieved_attrs);

	/* In what follows, do not risk leaking any PGresults. */
	PG_TRY();
	{
		res = PQexec(conn, sql.data);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			pgfdw_report_error(ERROR, res, conn, false, sql.data);
		PQclear(res);
		res = NULL;

		/* Retrieve and process rows a batch at a time. */
		for (;;)
		{
			char		fetch_sql[64];
			int			fetch_size;
			int			numrows;
			int			i;
			ListCell   *lc;

			/* Allow users to cancel long query */
			CHECK_FOR_INTERRUPTS();

			/*
			 * XXX possible future improvement: if rowstoskip is large, we
			 * could issue a MOVE rather than physically fetching the rows,
			 * then just adjust rowstoskip and samplerows appropriately.
			 */

			/* The fetch size is arbitrary, but shouldn't be enormous. */
			fetch_size = 100;
			foreach(lc, server->options)
			{
				DefElem    *def = (DefElem *) lfirst(lc);

				if (strcmp(def->defname, "fetch_size") == 0)
				{
					fetch_size = strtol(defGetString(def), NULL, 10);
					break;
				}
			}
			foreach(lc, table->options)
			{
				DefElem    *def = (DefElem *) lfirst(lc);

				if (strcmp(def->defname, "fetch_size") == 0)
				{
					fetch_size = strtol(defGetString(def), NULL, 10);
					break;
				}
			}

			/* Fetch some rows */
			snprintf(fetch_sql, sizeof(fetch_sql), "FETCH %d FROM c%u",
					 fetch_size, cursor_number);

			res = PQexec(conn, fetch_sql);
			/* On error, report the original query, not the FETCH. */
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
				pgfdw_report_error(ERROR, res, conn, false, sql.data);

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
			astate->rowstoskip = reservoir_get_next_S(&astate->rstate, astate->samplerows, targrows);

		if (astate->rowstoskip <= 0)
		{
			/* Choose a random reservoir element to replace. */
			pos = (int) (targrows * sampler_random_fract(astate->rstate.randstate));
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
													 astate->retrieved_attrs,
													   NULL,
													   astate->temp_cxt);

		MemoryContextSwitchTo(oldcontext);
	}
}

/*
 * Import a foreign schema
 */
static List *
postgresImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	List	   *commands = NIL;
	bool		import_collate = true;
	bool		import_default = false;
	bool		import_not_null = true;
	ForeignServer *server;
	UserMapping *mapping;
	PGconn	   *conn;
	StringInfoData buf;
	PGresult   *volatile res = NULL;
	int			numrows,
				i;
	ListCell   *lc;

	/* Parse statement options */
	foreach(lc, stmt->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "import_collate") == 0)
			import_collate = defGetBoolean(def);
		else if (strcmp(def->defname, "import_default") == 0)
			import_default = defGetBoolean(def);
		else if (strcmp(def->defname, "import_not_null") == 0)
			import_not_null = defGetBoolean(def);
		else
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname)));
	}

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	server = GetForeignServer(serverOid);
	mapping = GetUserMapping(GetUserId(), server->serverid);
	conn = GetConnection(mapping, false);

	/* Don't attempt to import collation if remote server hasn't got it */
	if (PQserverVersion(conn) < 90100)
		import_collate = false;

	/* Create workspace for strings */
	initStringInfo(&buf);

	/* In what follows, do not risk leaking any PGresults. */
	PG_TRY();
	{
		/* Check that the schema really exists */
		appendStringInfoString(&buf, "SELECT 1 FROM pg_catalog.pg_namespace WHERE nspname = ");
		deparseStringLiteral(&buf, stmt->remote_schema);

		res = PQexec(conn, buf.data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, buf.data);

		if (PQntuples(res) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_SCHEMA_NOT_FOUND),
			  errmsg("schema \"%s\" is not present on foreign server \"%s\"",
					 stmt->remote_schema, server->servername)));

		PQclear(res);
		res = NULL;
		resetStringInfo(&buf);

		/*
		 * Fetch all table data from this schema, possibly restricted by
		 * EXCEPT or LIMIT TO.  (We don't actually need to pay any attention
		 * to EXCEPT/LIMIT TO here, because the core code will filter the
		 * statements we return according to those lists anyway.  But it
		 * should save a few cycles to not process excluded tables in the
		 * first place.)
		 *
		 * Note: because we run the connection with search_path restricted to
		 * pg_catalog, the format_type() and pg_get_expr() outputs will always
		 * include a schema name for types/functions in other schemas, which
		 * is what we want.
		 */
		if (import_collate)
			appendStringInfoString(&buf,
								   "SELECT relname, "
								   "  attname, "
								   "  format_type(atttypid, atttypmod), "
								   "  attnotnull, "
								   "  pg_get_expr(adbin, adrelid), "
								   "  collname, "
								   "  collnsp.nspname "
								   "FROM pg_class c "
								   "  JOIN pg_namespace n ON "
								   "    relnamespace = n.oid "
								   "  LEFT JOIN pg_attribute a ON "
								   "    attrelid = c.oid AND attnum > 0 "
								   "      AND NOT attisdropped "
								   "  LEFT JOIN pg_attrdef ad ON "
								   "    adrelid = c.oid AND adnum = attnum "
								   "  LEFT JOIN pg_collation coll ON "
								   "    coll.oid = attcollation "
								   "  LEFT JOIN pg_namespace collnsp ON "
								   "    collnsp.oid = collnamespace ");
		else
			appendStringInfoString(&buf,
								   "SELECT relname, "
								   "  attname, "
								   "  format_type(atttypid, atttypmod), "
								   "  attnotnull, "
								   "  pg_get_expr(adbin, adrelid), "
								   "  NULL, NULL "
								   "FROM pg_class c "
								   "  JOIN pg_namespace n ON "
								   "    relnamespace = n.oid "
								   "  LEFT JOIN pg_attribute a ON "
								   "    attrelid = c.oid AND attnum > 0 "
								   "      AND NOT attisdropped "
								   "  LEFT JOIN pg_attrdef ad ON "
								   "    adrelid = c.oid AND adnum = attnum ");

		appendStringInfoString(&buf,
							   "WHERE c.relkind IN ('r', 'v', 'f', 'm') "
							   "  AND n.nspname = ");
		deparseStringLiteral(&buf, stmt->remote_schema);

		/* Apply restrictions for LIMIT TO and EXCEPT */
		if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO ||
			stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
		{
			bool		first_item = true;

			appendStringInfoString(&buf, " AND c.relname ");
			if (stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
				appendStringInfoString(&buf, "NOT ");
			appendStringInfoString(&buf, "IN (");

			/* Append list of table names within IN clause */
			foreach(lc, stmt->table_list)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);

				if (first_item)
					first_item = false;
				else
					appendStringInfoString(&buf, ", ");
				deparseStringLiteral(&buf, rv->relname);
			}
			appendStringInfoChar(&buf, ')');
		}

		/* Append ORDER BY at the end of query to ensure output ordering */
		appendStringInfoString(&buf, " ORDER BY c.relname, a.attnum");

		/* Fetch the data */
		res = PQexec(conn, buf.data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, buf.data);

		/* Process results */
		numrows = PQntuples(res);
		/* note: incrementation of i happens in inner loop's while() test */
		for (i = 0; i < numrows;)
		{
			char	   *tablename = PQgetvalue(res, i, 0);
			bool		first_item = true;

			resetStringInfo(&buf);
			appendStringInfo(&buf, "CREATE FOREIGN TABLE %s (\n",
							 quote_identifier(tablename));

			/* Scan all rows for this table */
			do
			{
				char	   *attname;
				char	   *typename;
				char	   *attnotnull;
				char	   *attdefault;
				char	   *collname;
				char	   *collnamespace;

				/* If table has no columns, we'll see nulls here */
				if (PQgetisnull(res, i, 1))
					continue;

				attname = PQgetvalue(res, i, 1);
				typename = PQgetvalue(res, i, 2);
				attnotnull = PQgetvalue(res, i, 3);
				attdefault = PQgetisnull(res, i, 4) ? (char *) NULL :
					PQgetvalue(res, i, 4);
				collname = PQgetisnull(res, i, 5) ? (char *) NULL :
					PQgetvalue(res, i, 5);
				collnamespace = PQgetisnull(res, i, 6) ? (char *) NULL :
					PQgetvalue(res, i, 6);

				if (first_item)
					first_item = false;
				else
					appendStringInfoString(&buf, ",\n");

				/* Print column name and type */
				appendStringInfo(&buf, "  %s %s",
								 quote_identifier(attname),
								 typename);

				/*
				 * Add column_name option so that renaming the foreign table's
				 * column doesn't break the association to the underlying
				 * column.
				 */
				appendStringInfoString(&buf, " OPTIONS (column_name ");
				deparseStringLiteral(&buf, attname);
				appendStringInfoChar(&buf, ')');

				/* Add COLLATE if needed */
				if (import_collate && collname != NULL && collnamespace != NULL)
					appendStringInfo(&buf, " COLLATE %s.%s",
									 quote_identifier(collnamespace),
									 quote_identifier(collname));

				/* Add DEFAULT if needed */
				if (import_default && attdefault != NULL)
					appendStringInfo(&buf, " DEFAULT %s", attdefault);

				/* Add NOT NULL if needed */
				if (import_not_null && attnotnull[0] == 't')
					appendStringInfoString(&buf, " NOT NULL");
			}
			while (++i < numrows &&
				   strcmp(PQgetvalue(res, i, 0), tablename) == 0);

			/*
			 * Add server name and table-level options.  We specify remote
			 * schema and table name as options (the latter to ensure that
			 * renaming the foreign table doesn't break the association).
			 */
			appendStringInfo(&buf, "\n) SERVER %s\nOPTIONS (",
							 quote_identifier(server->servername));

			appendStringInfoString(&buf, "schema_name ");
			deparseStringLiteral(&buf, stmt->remote_schema);
			appendStringInfoString(&buf, ", table_name ");
			deparseStringLiteral(&buf, tablename);

			appendStringInfoString(&buf, ");");

			commands = lappend(commands, pstrdup(buf.data));
		}

		/* Clean up */
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

	return commands;
}

/*
 * Assess whether the join between inner and outer relations can be pushed down
 * to the foreign server. As a side effect, save information we obtain in this
 * function to PgFdwRelationInfo passed in.
 *
 * Joins that satisfy conditions below are safe to push down.
 *
 * 1) Join type is INNER or OUTER (one of LEFT/RIGHT/FULL)
 * 2) Both outer and inner portions are safe to push-down
 * 3) All join conditions are safe to push down
 * 4) No relation has local filter (this can be relaxed for INNER JOIN, if we
 *	  can move unpushable clauses upwards in the join tree).
 */
static bool
foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel, JoinType jointype,
				RelOptInfo *outerrel, RelOptInfo *innerrel,
				JoinPathExtraData *extra)
{
	PgFdwRelationInfo *fpinfo;
	PgFdwRelationInfo *fpinfo_o;
	PgFdwRelationInfo *fpinfo_i;
	ListCell   *lc;
	List	   *joinclauses;
	List	   *otherclauses;

	/*
	 * We support pushing down INNER, LEFT, RIGHT and FULL OUTER joins.
	 * Constructing queries representing SEMI and ANTI joins is hard, hence
	 * not considered right now.
	 */
	if (jointype != JOIN_INNER && jointype != JOIN_LEFT &&
		jointype != JOIN_RIGHT && jointype != JOIN_FULL)
		return false;

	/*
	 * If either of the joining relations is marked as unsafe to pushdown, the
	 * join can not be pushed down.
	 */
	fpinfo = (PgFdwRelationInfo *) joinrel->fdw_private;
	fpinfo_o = (PgFdwRelationInfo *) outerrel->fdw_private;
	fpinfo_i = (PgFdwRelationInfo *) innerrel->fdw_private;
	if (!fpinfo_o || !fpinfo_o->pushdown_safe ||
		!fpinfo_i || !fpinfo_i->pushdown_safe)
		return false;

	/*
	 * If joining relations have local conditions, those conditions are
	 * required to be applied before joining the relations. Hence the join can
	 * not be pushed down.
	 */
	if (fpinfo_o->local_conds || fpinfo_i->local_conds)
		return false;

	/* Separate restrict list into join quals and quals on join relation */
	if (IS_OUTER_JOIN(jointype))
		extract_actual_join_clauses(extra->restrictlist, &joinclauses, &otherclauses);
	else
	{
		/*
		 * Unlike an outer join, for inner join, the join result contains only
		 * the rows which satisfy join clauses, similar to the other clause.
		 * Hence all clauses can be treated as other quals. This helps to push
		 * a join down to the foreign server even if some of its join quals
		 * are not safe to pushdown.
		 */
		otherclauses = extract_actual_clauses(extra->restrictlist, false);
		joinclauses = NIL;
	}

	/* Join quals must be safe to push down. */
	foreach(lc, joinclauses)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		if (!is_foreign_expr(root, joinrel, expr))
			return false;
	}

	/* Save the join clauses, for later use. */
	fpinfo->joinclauses = joinclauses;

	/*
	 * Other clauses are applied after the join has been performed and thus
	 * need not be all pushable. We will push those which can be pushed to
	 * reduce the number of rows fetched from the foreign server. Rest of them
	 * will be applied locally after fetching join result. Add them to fpinfo
	 * so that other joins involving this joinrel will know that this joinrel
	 * has local clauses.
	 */
	foreach(lc, otherclauses)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		if (!is_foreign_expr(root, joinrel, expr))
			fpinfo->local_conds = lappend(fpinfo->local_conds, expr);
		else
			fpinfo->remote_conds = lappend(fpinfo->remote_conds, expr);
	}

	fpinfo->outerrel = outerrel;
	fpinfo->innerrel = innerrel;
	fpinfo->jointype = jointype;

	/*
	 * If user is willing to estimate cost for a scan of either of the joining
	 * relations using EXPLAIN, he intends to estimate scans on that relation
	 * more accurately. Then, it makes sense to estimate the cost the join
	 * with that relation more accurately using EXPLAIN.
	 */
	fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate ||
		fpinfo_i->use_remote_estimate;

	/*
	 * Since both the joining relations come from the same server, the server
	 * level options should have same value for both the relations. Pick from
	 * any side.
	 */
	fpinfo->fdw_startup_cost = fpinfo_o->fdw_startup_cost;
	fpinfo->fdw_tuple_cost = fpinfo_o->fdw_tuple_cost;

	/*
	 * Set cached relation costs to some negative value, so that we can detect
	 * when they are set to some sensible costs, during one (usually the
	 * first) of the calls to estimate_path_cost_size().
	 */
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/* Mark that this join can be pushed down safely */
	fpinfo->pushdown_safe = true;

	/*
	 * Set fetch size to maximum of the joining sides, since we are expecting
	 * the rows returned by the join to be proportional to the relation sizes.
	 */
	if (fpinfo_o->fetch_size > fpinfo_i->fetch_size)
		fpinfo->fetch_size = fpinfo_o->fetch_size;
	else
		fpinfo->fetch_size = fpinfo_i->fetch_size;

	/*
	 * Pull the other remote conditions from the joining relations into join
	 * clauses or other remote clauses (remote_conds) of this relation. This
	 * avoids building subqueries at every join step.
	 *
	 * For an inner join, clauses from both the relations are added to the
	 * other remote clauses. For an OUTER join, the clauses from the outer
	 * side are added to remote_conds since those can be evaluated after the
	 * join is evaluated. The clauses from inner side are added to the
	 * joinclauses, since they need to evaluated while constructing the join.
	 *
	 * The joining sides can not have local conditions, thus no need to test
	 * shippability of the clauses being pulled up.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
										  list_copy(fpinfo_i->remote_conds));
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
										  list_copy(fpinfo_o->remote_conds));
			break;

		case JOIN_LEFT:
			fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
										  list_copy(fpinfo_i->remote_conds));
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
										  list_copy(fpinfo_o->remote_conds));
			break;

		case JOIN_RIGHT:
			fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
										  list_copy(fpinfo_o->remote_conds));
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
										  list_copy(fpinfo_i->remote_conds));
			break;

		case JOIN_FULL:
			fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
										  list_copy(fpinfo_i->remote_conds));
			fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
										  list_copy(fpinfo_o->remote_conds));
			break;

		default:
			/* Should not happen, we have just check this above */
			elog(ERROR, "unsupported join type %d", jointype);
	}

	/*
	 * Set the string describing this join relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.
	 */
	fpinfo->relation_name = makeStringInfo();
	appendStringInfo(fpinfo->relation_name, "(%s) %s JOIN (%s)",
					 fpinfo_o->relation_name->data,
					 get_jointype_name(fpinfo->jointype),
					 fpinfo_i->relation_name->data);

	return true;
}

static void
add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
								Path *epq_path)
{
	List	   *useful_pathkeys_list = NIL;		/* List of all pathkeys */
	ListCell   *lc;

	useful_pathkeys_list = get_useful_pathkeys_for_relation(root, rel);

	/* Create one path for each set of pathkeys we found above. */
	foreach(lc, useful_pathkeys_list)
	{
		double		rows;
		int			width;
		Cost		startup_cost;
		Cost		total_cost;
		List	   *useful_pathkeys = lfirst(lc);

		estimate_path_cost_size(root, rel, NIL, useful_pathkeys,
								&rows, &width, &startup_cost, &total_cost);

		add_path(rel, (Path *)
				 create_foreignscan_path(root, rel,
										 NULL,
										 rows,
										 startup_cost,
										 total_cost,
										 useful_pathkeys,
										 NULL,
										 epq_path,
										 NIL));
	}
}

/*
 * postgresGetForeignJoinPaths
 *		Add possible ForeignPath to joinrel, if join is safe to push down.
 */
static void
postgresGetForeignJoinPaths(PlannerInfo *root,
							RelOptInfo *joinrel,
							RelOptInfo *outerrel,
							RelOptInfo *innerrel,
							JoinType jointype,
							JoinPathExtraData *extra)
{
	PgFdwRelationInfo *fpinfo;
	ForeignPath *joinpath;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	Path	   *epq_path;		/* Path to create plan to be executed when
								 * EvalPlanQual gets triggered. */

	/*
	 * Skip if this join combination has been considered already.
	 */
	if (joinrel->fdw_private)
		return;

	/*
	 * Create unfinished PgFdwRelationInfo entry which is used to indicate
	 * that the join relation is already considered, so that we won't waste
	 * time in judging safety of join pushdown and adding the same paths again
	 * if found safe. Once we know that this join can be pushed down, we fill
	 * the entry.
	 */
	fpinfo = (PgFdwRelationInfo *) palloc0(sizeof(PgFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	joinrel->fdw_private = fpinfo;
	/* attrs_used is only for base relations. */
	fpinfo->attrs_used = NULL;

	/*
	 * In case there is a possibility that EvalPlanQual will be executed, we
	 * should be able to reconstruct the row, from base relations applying all
	 * the conditions. We create a local plan from a suitable local path
	 * available in the path list. In case such a path doesn't exist, we can
	 * not push the join to the foreign server since we won't be able to
	 * reconstruct the row for EvalPlanQual(). Find an alternative local path
	 * before we add ForeignPath, lest the new path would kick possibly the
	 * only local path. Do this before calling foreign_join_ok(), since that
	 * function updates fpinfo and marks it as pushable if the join is found
	 * to be pushable.
	 */
	if (root->parse->commandType == CMD_DELETE ||
		root->parse->commandType == CMD_UPDATE ||
		root->rowMarks)
	{
		epq_path = GetExistingLocalJoinPath(joinrel);
		if (!epq_path)
		{
			elog(DEBUG3, "could not push down foreign join because a local path suitable for EPQ checks was not found");
			return;
		}
	}
	else
		epq_path = NULL;

	if (!foreign_join_ok(root, joinrel, jointype, outerrel, innerrel, extra))
	{
		/* Free path required for EPQ if we copied one; we don't need it now */
		if (epq_path)
			pfree(epq_path);
		return;
	}

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path. The best we can do for these
	 * conditions is to estimate selectivity on the basis of local statistics.
	 * The local conditions are applied after the join has been computed on
	 * the remote side like quals in WHERE clause, so pass jointype as
	 * JOIN_INNER.
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 0,
													 JOIN_INNER,
													 NULL);
	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/*
	 * If we are going to estimate the costs using EXPLAIN, we will need
	 * connection information. Fill it here.
	 */
	if (fpinfo->use_remote_estimate)
		fpinfo->user = GetUserMappingById(joinrel->umid);
	else
	{
		fpinfo->user = NULL;

		/*
		 * If we are going to estimate costs locally, estimate the join clause
		 * selectivity here while we have special join info.
		 */
		fpinfo->joinclause_sel = clauselist_selectivity(root, fpinfo->joinclauses,
														0, fpinfo->jointype,
														extra->sjinfo);

	}
	fpinfo->server = GetForeignServer(joinrel->serverid);

	/* Estimate costs for bare join relation */
	estimate_path_cost_size(root, joinrel, NIL, NIL, &rows,
							&width, &startup_cost, &total_cost);
	/* Now update this information in the joinrel */
	joinrel->rows = rows;
	joinrel->reltarget->width = width;
	fpinfo->rows = rows;
	fpinfo->width = width;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	/*
	 * Create a new join path and add it to the joinrel which represents a
	 * join between foreign tables.
	 */
	joinpath = create_foreignscan_path(root,
									   joinrel,
									   NULL,	/* default pathtarget */
									   rows,
									   startup_cost,
									   total_cost,
									   NIL,		/* no pathkeys */
									   NULL,	/* no required_outer */
									   epq_path,
									   NULL);	/* no fdw_private */

	/* Add generated path into joinrel by add_path(). */
	add_path(joinrel, (Path *) joinpath);

	/* Consider pathkeys for the join relation */
	add_paths_with_pathkeys_for_rel(root, joinrel, epq_path);

	/* XXX Consider parameterized paths for the join relation */
}

/*
 * Create a tuple from the specified row of the PGresult.
 *
 * rel is the local representation of the foreign table, attinmeta is
 * conversion data for the rel's tupdesc, and retrieved_attrs is an
 * integer list of the table column numbers present in the PGresult.
 * temp_context is a working context that can be reset after each tuple.
 */
static HeapTuple
make_tuple_from_result_row(PGresult *res,
						   int row,
						   Relation rel,
						   AttInMetadata *attinmeta,
						   List *retrieved_attrs,
						   ForeignScanState *fsstate,
						   MemoryContext temp_context)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *nulls;
	ItemPointer ctid = NULL;
	ConversionLocation errpos;
	ErrorContextCallback errcallback;
	MemoryContext oldcontext;
	ListCell   *lc;
	int			j;

	Assert(row < PQntuples(res));

	/*
	 * Do the following work in a temp context that we reset after each tuple.
	 * This cleans up not only the data we have direct access to, but any
	 * cruft the I/O functions might leak.
	 */
	oldcontext = MemoryContextSwitchTo(temp_context);

	if (rel)
		tupdesc = RelationGetDescr(rel);
	else
	{
		PgFdwScanState *fdw_sstate;

		Assert(fsstate);
		fdw_sstate = (PgFdwScanState *) fsstate->fdw_state;
		tupdesc = fdw_sstate->tupdesc;
	}

	values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));
	/* Initialize to nulls for any columns not present in result */
	memset(nulls, true, tupdesc->natts * sizeof(bool));

	/*
	 * Set up and install callback to report where conversion error occurs.
	 */
	errpos.rel = rel;
	errpos.cur_attno = 0;
	errpos.fsstate = fsstate;
	errcallback.callback = conversion_error_callback;
	errcallback.arg = (void *) &errpos;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * i indexes columns in the relation, j indexes columns in the PGresult.
	 */
	j = 0;
	foreach(lc, retrieved_attrs)
	{
		int			i = lfirst_int(lc);
		char	   *valstr;

		/* fetch next column's textual value */
		if (PQgetisnull(res, row, j))
			valstr = NULL;
		else
			valstr = PQgetvalue(res, row, j);

		/* convert value to internal representation */
		errpos.cur_attno = i;
		if (i > 0)
		{
			/* ordinary column */
			Assert(i <= tupdesc->natts);
			nulls[i - 1] = (valstr == NULL);
			/* Apply the input function even to nulls, to support domains */
			values[i - 1] = InputFunctionCall(&attinmeta->attinfuncs[i - 1],
											  valstr,
											  attinmeta->attioparams[i - 1],
											  attinmeta->atttypmods[i - 1]);
		}
		else if (i == SelfItemPointerAttributeNumber)
		{
			/* ctid --- note we ignore any other system column in result */
			if (valstr != NULL)
			{
				Datum		datum;

				datum = DirectFunctionCall1(tidin, CStringGetDatum(valstr));
				ctid = (ItemPointer) DatumGetPointer(datum);
			}
		}
		errpos.cur_attno = 0;

		j++;
	}

	/* Uninstall error context callback. */
	error_context_stack = errcallback.previous;

	/*
	 * Check we got the expected number of columns.  Note: j == 0 and
	 * PQnfields == 1 is expected, since deparse emits a NULL if no columns.
	 */
	if (j > 0 && j != PQnfields(res))
		elog(ERROR, "remote query result does not match the foreign table");

	/*
	 * Build the result tuple in caller's memory context.
	 */
	MemoryContextSwitchTo(oldcontext);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * If we have a CTID to return, install it in both t_self and t_ctid.
	 * t_self is the normal place, but if the tuple is converted to a
	 * composite Datum, t_self will be lost; setting t_ctid allows CTID to be
	 * preserved during EvalPlanQual re-evaluations (see ROW_MARK_COPY code).
	 */
	if (ctid)
		tuple->t_self = tuple->t_data->t_ctid = *ctid;

	/* Clean up */
	MemoryContextReset(temp_context);

	return tuple;
}

/*
 * Callback function which is called when error occurs during column value
 * conversion.  Print names of column and relation.
 */
static void
conversion_error_callback(void *arg)
{
	const char *attname = NULL;
	const char *relname = NULL;
	ConversionLocation *errpos = (ConversionLocation *) arg;

	if (errpos->rel)
	{
		/* error occurred in a scan against a foreign table */
		TupleDesc	tupdesc = RelationGetDescr(errpos->rel);

		if (errpos->cur_attno > 0 && errpos->cur_attno <= tupdesc->natts)
			attname = NameStr(tupdesc->attrs[errpos->cur_attno - 1]->attname);
		else if (errpos->cur_attno == SelfItemPointerAttributeNumber)
			attname = "ctid";

		relname = RelationGetRelationName(errpos->rel);
	}
	else
	{
		/* error occurred in a scan against a foreign join */
		ForeignScanState *fsstate = errpos->fsstate;
		ForeignScan *fsplan = (ForeignScan *) fsstate->ss.ps.plan;
		EState	   *estate = fsstate->ss.ps.state;
		TargetEntry *tle;
		Var		   *var;
		RangeTblEntry *rte;

		Assert(IsA(fsplan, ForeignScan));
		tle = (TargetEntry *) list_nth(fsplan->fdw_scan_tlist,
									   errpos->cur_attno - 1);
		Assert(IsA(tle, TargetEntry));
		var = (Var *) tle->expr;
		Assert(IsA(var, Var));

		rte = rt_fetch(var->varno, estate->es_range_table);
		relname = get_rel_name(rte->relid);
		attname = get_relid_attribute_name(rte->relid, var->varattno);
	}

	if (attname && relname)
		errcontext("column \"%s\" of foreign table \"%s\"", attname, relname);
}

/*
 * Find an equivalence class member expression, all of whose Vars, come from
 * the indicated relation.
 */
extern Expr *
find_em_expr_for_rel(EquivalenceClass *ec, RelOptInfo *rel)
{
	ListCell   *lc_em;

	foreach(lc_em, ec->ec_members)
	{
		EquivalenceMember *em = lfirst(lc_em);

		if (bms_is_subset(em->em_relids, rel->relids))
		{
			/*
			 * If there is more than one equivalence member whose Vars are
			 * taken entirely from this relation, we'll be content to choose
			 * any one of those.
			 */
			return em->em_expr;
		}
	}

	/* We didn't find any suitable equivalence class expression */
	return NULL;
}
