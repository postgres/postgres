/*-------------------------------------------------------------------------
 *
 * postgres_fdw.c
 *		  Foreign-data wrapper for remote PostgreSQL servers
 *
 * Portions Copyright (c) 2012-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/postgres_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opfamily.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "postgres_fdw.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/selfuncs.h"

/* source-code-compatibility hacks for pull_varnos() API change */
#define make_restrictinfo(a,b,c,d,e,f,g,h,i) make_restrictinfo_new(a,b,c,d,e,f,g,h,i)

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
	/* Integer list of attribute numbers retrieved by the SELECT */
	FdwScanPrivateRetrievedAttrs,
	/* Integer representing the desired fetch_size */
	FdwScanPrivateFetchSize,

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

	/* for update row movement if subplan result rel */
	struct PgFdwModifyState *aux_fmstate;	/* foreign-insert state, if
											 * created */
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
	Relation	resultRel;		/* relcache entry for the target relation */
	AttrNumber *attnoMap;		/* array of attnums of input user columns */
	AttrNumber	ctidAttno;		/* attnum of input ctid column */
	AttrNumber	oidAttno;		/* attnum of input oid column */
	bool		hasSystemCols;	/* are there system columns of resultRel? */

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
 * This enum describes what's kept in the fdw_private list for a ForeignPath.
 * We store:
 *
 * 1) Boolean flag showing if the remote query has the final sort
 * 2) Boolean flag showing if the remote query has the LIMIT clause
 */
enum FdwPathPrivateIndex
{
	/* has-final-sort flag (as an integer Value node) */
	FdwPathPrivateHasFinalSort,
	/* has-limit flag (as an integer Value node) */
	FdwPathPrivateHasLimit
};

/* Struct for extra information passed to estimate_path_cost_size() */
typedef struct
{
	PathTarget *target;
	bool		has_final_sort;
	bool		has_limit;
	double		limit_tuples;
	int64		count_est;
	int64		offset_est;
} PgFdwPathExtraData;

/*
 * Identify the attribute where data conversion fails.
 */
typedef struct ConversionLocation
{
	AttrNumber	cur_attno;		/* attribute number being processed, or 0 */
	Relation	rel;			/* foreign table being processed, or NULL */
	ForeignScanState *fsstate;	/* plan node being processed, or NULL */
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
										   RelOptInfo *foreignrel,
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
static void postgresBeginForeignInsert(ModifyTableState *mtstate,
									   ResultRelInfo *resultRelInfo);
static void postgresEndForeignInsert(EState *estate,
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
static void postgresGetForeignUpperPaths(PlannerInfo *root,
										 UpperRelationKind stage,
										 RelOptInfo *input_rel,
										 RelOptInfo *output_rel,
										 void *extra);

/*
 * Helper functions
 */
static void estimate_path_cost_size(PlannerInfo *root,
									RelOptInfo *foreignrel,
									List *param_join_conds,
									List *pathkeys,
									PgFdwPathExtraData *fpextra,
									double *p_rows, int *p_width,
									Cost *p_startup_cost, Cost *p_total_cost);
static void get_remote_estimate(const char *sql,
								PGconn *conn,
								double *rows,
								int *width,
								Cost *startup_cost,
								Cost *total_cost);
static void adjust_foreign_grouping_path_cost(PlannerInfo *root,
											  List *pathkeys,
											  double retrieved_rows,
											  double width,
											  double limit_tuples,
											  Cost *p_startup_cost,
											  Cost *p_run_cost);
static bool ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
									  EquivalenceClass *ec, EquivalenceMember *em,
									  void *arg);
static void create_cursor(ForeignScanState *node);
static void fetch_more_data(ForeignScanState *node);
static void close_cursor(PGconn *conn, unsigned int cursor_number);
static PgFdwModifyState *create_foreign_modify(EState *estate,
											   RangeTblEntry *rte,
											   ResultRelInfo *resultRelInfo,
											   CmdType operation,
											   Plan *subplan,
											   char *query,
											   List *target_attrs,
											   bool has_returning,
											   List *retrieved_attrs);
static TupleTableSlot *execute_foreign_modify(EState *estate,
											  ResultRelInfo *resultRelInfo,
											  CmdType operation,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static void prepare_foreign_modify(PgFdwModifyState *fmstate);
static const char **convert_prep_stmt_params(PgFdwModifyState *fmstate,
											 ItemPointer tupleid,
											 TupleTableSlot *slot);
static void store_returning_result(PgFdwModifyState *fmstate,
								   TupleTableSlot *slot, PGresult *res);
static void finish_foreign_modify(PgFdwModifyState *fmstate);
static List *build_remote_returning(Index rtindex, Relation rel,
									List *returningList);
static void rebuild_fdw_scan_tlist(ForeignScan *fscan, List *tlist);
static void execute_dml_stmt(ForeignScanState *node);
static TupleTableSlot *get_returning_data(ForeignScanState *node);
static void init_returning_filter(PgFdwDirectModifyState *dmstate,
								  List *fdw_scan_tlist,
								  Index rtindex);
static TupleTableSlot *apply_returning_filter(PgFdwDirectModifyState *dmstate,
											  TupleTableSlot *slot,
											  EState *estate);
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
static int	postgresAcquireSampleRowsFunc(Relation relation, int elevel,
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
static bool foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
								Node *havingQual);
static List *get_useful_pathkeys_for_relation(PlannerInfo *root,
											  RelOptInfo *rel);
static List *get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel);
static void add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
											Path *epq_path);
static void add_foreign_grouping_paths(PlannerInfo *root,
									   RelOptInfo *input_rel,
									   RelOptInfo *grouped_rel,
									   GroupPathExtraData *extra);
static void add_foreign_ordered_paths(PlannerInfo *root,
									  RelOptInfo *input_rel,
									  RelOptInfo *ordered_rel);
static void add_foreign_final_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *final_rel,
									FinalPathExtraData *extra);
static void apply_server_options(PgFdwRelationInfo *fpinfo);
static void apply_table_options(PgFdwRelationInfo *fpinfo);
static void merge_fdw_options(PgFdwRelationInfo *fpinfo,
							  const PgFdwRelationInfo *fpinfo_o,
							  const PgFdwRelationInfo *fpinfo_i);


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
	routine->BeginForeignInsert = postgresBeginForeignInsert;
	routine->EndForeignInsert = postgresEndForeignInsert;
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

	/* Support functions for upper relation push-down */
	routine->GetForeignUpperPaths = postgresGetForeignUpperPaths;

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

	/*
	 * We use PgFdwRelationInfo to pass various information to subsequent
	 * functions.
	 */
	fpinfo = (PgFdwRelationInfo *) palloc0(sizeof(PgFdwRelationInfo));
	baserel->fdw_private = (void *) fpinfo;

	/* Base foreign tables need to be pushed down always. */
	fpinfo->pushdown_safe = true;

	/* Look up foreign-table catalog info. */
	fpinfo->table = GetForeignTable(foreigntableid);
	fpinfo->server = GetForeignServer(fpinfo->table->serverid);

	/*
	 * Extract user-settable option values.  Note that per-table settings of
	 * use_remote_estimate and fetch_size override per-server settings of
	 * them, respectively.
	 */
	fpinfo->use_remote_estimate = false;
	fpinfo->fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
	fpinfo->fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;
	fpinfo->shippable_extensions = NIL;
	fpinfo->fetch_size = 100;

	apply_server_options(fpinfo);
	apply_table_options(fpinfo);

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
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

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
	 * Set # of retrieved rows and cached relation costs to some negative
	 * value, so that we can detect when they are set to some sensible values,
	 * during one (usually the first) of the calls to estimate_path_cost_size.
	 */
	fpinfo->retrieved_rows = -1;
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
		estimate_path_cost_size(root, baserel, NIL, NIL, NULL,
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
		estimate_path_cost_size(root, baserel, NIL, NIL, NULL,
								&fpinfo->rows, &fpinfo->width,
								&fpinfo->startup_cost, &fpinfo->total_cost);
	}

	/*
	 * fpinfo->relation_name gets the numeric rangetable index of the foreign
	 * table RTE.  (If this query gets EXPLAIN'd, we'll convert that to a
	 * human-readable string at that time.)
	 */
	fpinfo->relation_name = psprintf("%u", baserel->relid);

	/* No outer and inner relations. */
	fpinfo->make_outerrel_subquery = false;
	fpinfo->make_innerrel_subquery = false;
	fpinfo->lower_subquery_rels = NULL;
	/* Set the relation index. */
	fpinfo->relation_index = baserel->relid;
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
 *
 * This function returns a list of potentially-useful equivalence classes,
 * but it does not guarantee that an EquivalenceMember exists which contains
 * Vars only from the given relation.  For example, given ft1 JOIN t1 ON
 * ft1.x + t1.x = 0, this function will say that the equivalence class
 * containing ft1.x + t1.x is potentially useful.  Supposing ft1 is remote and
 * t1 is local (or on a different server), it will turn out that no useful
 * ORDER BY clause can be generated.  It's not our job to figure that out
 * here; we're only interested in identifying relevant ECs.
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
	if (IS_OTHER_REL(rel))
	{
		Assert(!bms_is_empty(rel->top_parent_relids));
		relids = rel->top_parent_relids;
	}
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
		 * distribute_qual_to_rels.
		 *
		 * We want to identify which side of this merge-joinable clause
		 * contains columns from the relation produced by this RelOptInfo. We
		 * test for overlap, not containment, because there could be extra
		 * relations on either side.  For example, suppose we've got something
		 * like ((A JOIN B ON A.x = B.x) JOIN C ON A.y = C.y) LEFT JOIN D ON
		 * A.y = D.y.  The input rel might be the joinrel between A and B, and
		 * we'll consider the join clause A.y = D.y. relids contains a
		 * relation not involved in the join class (B) and the equivalence
		 * class for the left-hand side of the clause contains a relation not
		 * involved in the input rel (C).  Despite the fact that we have only
		 * overlap and not containment in either direction, A.y is potentially
		 * useful as a sort column.
		 *
		 * Note that it's even possible that relids overlaps neither side of
		 * the join clause.  For example, consider A LEFT JOIN B ON A.x = B.x
		 * AND A.x = 1.  The clause A.x = 1 will appear in B's joininfo list,
		 * but overlaps neither side of B.  In that case, we just skip this
		 * join clause, since it doesn't suggest a useful sort order for this
		 * relation.
		 */
		if (bms_overlap(relids, restrictinfo->right_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->right_ec);
		else if (bms_overlap(relids, restrictinfo->left_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->left_ec);
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
	fpinfo->qp_is_pushdown_safe = false;
	if (root->query_pathkeys)
	{
		bool		query_pathkeys_ok = true;

		foreach(lc, root->query_pathkeys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(lc);

			/*
			 * The planner and executor don't have any clever strategy for
			 * taking data sorted by a prefix of the query's pathkeys and
			 * getting it to be sorted by all of those pathkeys. We'll just
			 * end up resorting the entire data set.  So, unless we can push
			 * down all of the query pathkeys, forget it.
			 */
			if (!is_foreign_pathkey(root, rel, pathkey))
			{
				query_pathkeys_ok = false;
				break;
			}
		}

		if (query_pathkeys_ok)
		{
			useful_pathkeys_list = list_make1(list_copy(root->query_pathkeys));
			fpinfo->qp_is_pushdown_safe = true;
		}
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
		PathKey    *pathkey;

		/* If redundant with what we did above, skip it. */
		if (cur_ec == query_ec)
			continue;

		/* Can't push down the sort if the EC's opfamily is not shippable. */
		if (!is_shippable(linitial_oid(cur_ec->ec_opfamilies),
						  OperatorFamilyRelationId, fpinfo))
			continue;

		/* If no pushable expression for this rel, skip it. */
		if (find_em_for_rel(root, cur_ec, rel) == NULL)
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
	 *
	 * Although this path uses no join clauses, it could still have required
	 * parameterization due to LATERAL refs in its tlist.
	 */
	path = create_foreignscan_path(root, baserel,
								   NULL,	/* default pathtarget */
								   fpinfo->rows,
								   fpinfo->startup_cost,
								   fpinfo->total_cost,
								   NIL, /* no pathkeys */
								   baserel->lateral_relids,
								   NULL,	/* no extra plan */
								   NIL);	/* no fdw_private list */
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
								param_info->ppi_clauses, NIL, NULL,
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
									   NIL, /* no pathkeys */
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
	List	   *remote_exprs = NIL;
	List	   *local_exprs = NIL;
	List	   *params_list = NIL;
	List	   *fdw_scan_tlist = NIL;
	List	   *fdw_recheck_quals = NIL;
	List	   *retrieved_attrs;
	StringInfoData sql;
	bool		has_final_sort = false;
	bool		has_limit = false;
	ListCell   *lc;

	/*
	 * Get FDW private data created by postgresGetForeignUpperPaths(), if any.
	 */
	if (best_path->fdw_private)
	{
		has_final_sort = intVal(list_nth(best_path->fdw_private,
										 FdwPathPrivateHasFinalSort));
		has_limit = intVal(list_nth(best_path->fdw_private,
									FdwPathPrivateHasLimit));
	}

	if (IS_SIMPLE_REL(foreignrel))
	{
		/*
		 * For base relations, set scan_relid as the relid of the relation.
		 */
		scan_relid = foreignrel->relid;

		/*
		 * In a base-relation scan, we must apply the given scan_clauses.
		 *
		 * Separate the scan_clauses into those that can be executed remotely
		 * and those that can't.  baserestrictinfo clauses that were
		 * previously determined to be safe or unsafe by classifyConditions
		 * are found in fpinfo->remote_conds and fpinfo->local_conds. Anything
		 * else in the scan_clauses list will be a join clause, which we have
		 * to check for remote-safety.
		 *
		 * Note: the join clauses we see here should be the exact same ones
		 * previously examined by postgresGetForeignPaths.  Possibly it'd be
		 * worth passing forward the classification work done then, rather
		 * than repeating it here.
		 *
		 * This code must match "extract_actual_clauses(scan_clauses, false)"
		 * except for the additional decision about remote versus local
		 * execution.
		 */
		foreach(lc, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			/* Ignore any pseudoconstants, they're dealt with elsewhere */
			if (rinfo->pseudoconstant)
				continue;

			if (list_member_ptr(fpinfo->remote_conds, rinfo))
				remote_exprs = lappend(remote_exprs, rinfo->clause);
			else if (list_member_ptr(fpinfo->local_conds, rinfo))
				local_exprs = lappend(local_exprs, rinfo->clause);
			else if (is_foreign_expr(root, foreignrel, rinfo->clause))
				remote_exprs = lappend(remote_exprs, rinfo->clause);
			else
				local_exprs = lappend(local_exprs, rinfo->clause);
		}

		/*
		 * For a base-relation scan, we have to support EPQ recheck, which
		 * should recheck all the remote quals.
		 */
		fdw_recheck_quals = remote_exprs;
	}
	else
	{
		/*
		 * Join relation or upper relation - set scan_relid to 0.
		 */
		scan_relid = 0;

		/*
		 * For a join rel, baserestrictinfo is NIL and we are not considering
		 * parameterization right now, so there should be no scan_clauses for
		 * a joinrel or an upper rel either.
		 */
		Assert(!scan_clauses);

		/*
		 * Instead we get the conditions to apply from the fdw_private
		 * structure.
		 */
		remote_exprs = extract_actual_clauses(fpinfo->remote_conds, false);
		local_exprs = extract_actual_clauses(fpinfo->local_conds, false);

		/*
		 * We leave fdw_recheck_quals empty in this case, since we never need
		 * to apply EPQ recheck clauses.  In the case of a joinrel, EPQ
		 * recheck is handled elsewhere --- see postgresGetForeignJoinPaths().
		 * If we're planning an upperrel (ie, remote grouping or aggregation)
		 * then there's no EPQ to do because SELECT FOR UPDATE wouldn't be
		 * allowed, and indeed we *can't* put the remote clauses into
		 * fdw_recheck_quals because the unaggregated Vars won't be available
		 * locally.
		 */

		/* Build the list of columns to be fetched from the foreign server. */
		fdw_scan_tlist = build_tlist_to_deparse(foreignrel);

		/*
		 * Ensure that the outer plan produces a tuple whose descriptor
		 * matches our scan tuple slot.  Also, remove the local conditions
		 * from outer plan's quals, lest they be evaluated twice, once by the
		 * local plan and once by the scan.
		 */
		if (outer_plan)
		{
			ListCell   *lc;

			/*
			 * Right now, we only consider grouping and aggregation beyond
			 * joins. Queries involving aggregates or grouping do not require
			 * EPQ mechanism, hence should not have an outer plan here.
			 */
			Assert(!IS_UPPER_REL(foreignrel));

			/*
			 * First, update the plan's qual list if possible.  In some cases
			 * the quals might be enforced below the topmost plan level, in
			 * which case we'll fail to remove them; it's not worth working
			 * harder than this.
			 */
			foreach(lc, local_exprs)
			{
				Node	   *qual = lfirst(lc);

				outer_plan->qual = list_delete(outer_plan->qual, qual);

				/*
				 * For an inner join the local conditions of foreign scan plan
				 * can be part of the joinquals as well.  (They might also be
				 * in the mergequals or hashquals, but we can't touch those
				 * without breaking the plan.)
				 */
				if (IsA(outer_plan, NestLoop) ||
					IsA(outer_plan, MergeJoin) ||
					IsA(outer_plan, HashJoin))
				{
					Join	   *join_plan = (Join *) outer_plan;

					if (join_plan->jointype == JOIN_INNER)
						join_plan->joinqual = list_delete(join_plan->joinqual,
														  qual);
				}
			}

			/*
			 * Now fix the subplan's tlist --- this might result in inserting
			 * a Result node atop the plan tree.
			 */
			outer_plan = change_plan_targetlist(outer_plan, fdw_scan_tlist,
												best_path->path.parallel_safe);
		}
	}

	/*
	 * Build the query string to be sent for execution, and identify
	 * expressions to be sent as parameters.
	 */
	initStringInfo(&sql);
	deparseSelectStmtForRel(&sql, root, foreignrel, fdw_scan_tlist,
							remote_exprs, best_path->path.pathkeys,
							has_final_sort, has_limit, false,
							&retrieved_attrs, &params_list);

	/* Remember remote_exprs for possible use by postgresPlanDirectModify */
	fpinfo->final_remote_exprs = remote_exprs;

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match order in enum FdwScanPrivateIndex.
	 */
	fdw_private = list_make3(makeString(sql.data),
							 retrieved_attrs,
							 makeInteger(fpinfo->fetch_size));
	if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
		fdw_private = lappend(fdw_private,
							  makeString(fpinfo->relation_name));

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
							fdw_recheck_quals,
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
	RangeTblEntry *rte;
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	int			rtindex;
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
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does.  In case of a join or aggregate, use the
	 * lowest-numbered member RTE as a representative; we would get the same
	 * result from any.
	 */
	if (fsplan->scan.scanrelid > 0)
		rtindex = fsplan->scan.scanrelid;
	else
		rtindex = bms_next_member(fsplan->fs_relids, -1);
	rte = exec_rt_fetch(rtindex, estate);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	table = GetForeignTable(rte->relid);
	user = GetUserMapping(userid, table->serverid);

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
											   ALLOCSET_DEFAULT_SIZES);
	fsstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_SIZES);

	/*
	 * Get info we'll need for converting data fetched from the foreign server
	 * into local representation and error reporting during that process.
	 */
	if (fsplan->scan.scanrelid > 0)
	{
		fsstate->rel = node->ss.ss_currentRelation;
		fsstate->tupdesc = RelationGetDescr(fsstate->rel);
	}
	else
	{
		fsstate->rel = NULL;
		fsstate->tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	}

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
	ExecStoreHeapTuple(fsstate->tuples[fsstate->next_tuple++],
					   slot,
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
	res = pgfdw_exec_query(fsstate->conn, sql);
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
	List	   *withCheckOptionList = NIL;
	List	   *returningList = NIL;
	List	   *retrieved_attrs = NIL;
	bool		doNothing = false;

	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	rel = table_open(rte->relid, NoLock);

	/*
	 * In an INSERT, we transmit all columns that are defined in the foreign
	 * table.  In an UPDATE, if there are BEFORE ROW UPDATE triggers on the
	 * foreign table, we transmit all columns like INSERT; else we transmit
	 * only columns that were explicitly targets of the UPDATE, so as to avoid
	 * unnecessary data transmission.  (We can't do that for INSERT since we
	 * would miss sending default values for columns not listed in the source
	 * statement, and for UPDATE if there are BEFORE ROW UPDATE triggers since
	 * those triggers might change values for non-target columns, in which
	 * case we would miss sending changed values for those columns.)
	 */
	if (operation == CMD_INSERT ||
		(operation == CMD_UPDATE &&
		 rel->trigdesc &&
		 rel->trigdesc->trig_update_before_row))
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		int			col;
		RelOptInfo *rel = find_base_rel(root, resultRelation);
		Bitmapset  *allUpdatedCols = get_rel_all_updated_cols(root, rel);

		col = -1;
		while ((col = bms_next_member(allUpdatedCols, col)) >= 0)
		{
			/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
			AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

			if (attno <= InvalidAttrNumber) /* shouldn't happen */
				elog(ERROR, "system-column update is not supported");
			targetAttrs = lappend_int(targetAttrs, attno);
		}
	}

	/*
	 * Extract the relevant WITH CHECK OPTION list if any.
	 */
	if (plan->withCheckOptionLists)
		withCheckOptionList = (List *) list_nth(plan->withCheckOptionLists,
												subplan_index);

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
			deparseInsertSql(&sql, rte, resultRelation, rel,
							 targetAttrs, doNothing,
							 withCheckOptionList, returningList,
							 &retrieved_attrs);
			break;
		case CMD_UPDATE:
			deparseUpdateSql(&sql, rte, resultRelation, rel,
							 targetAttrs,
							 withCheckOptionList, returningList,
							 &retrieved_attrs);
			break;
		case CMD_DELETE:
			deparseDeleteSql(&sql, rte, resultRelation, rel,
							 returningList,
							 &retrieved_attrs);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	table_close(rel, NoLock);

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
	char	   *query;
	List	   *target_attrs;
	bool		has_returning;
	List	   *retrieved_attrs;
	RangeTblEntry *rte;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  resultRelInfo->ri_FdwState
	 * stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Deconstruct fdw_private data. */
	query = strVal(list_nth(fdw_private,
							FdwModifyPrivateUpdateSql));
	target_attrs = (List *) list_nth(fdw_private,
									 FdwModifyPrivateTargetAttnums);
	has_returning = intVal(list_nth(fdw_private,
									FdwModifyPrivateHasReturning));
	retrieved_attrs = (List *) list_nth(fdw_private,
										FdwModifyPrivateRetrievedAttrs);

	/* Find RTE. */
	rte = exec_rt_fetch(resultRelInfo->ri_RangeTableIndex,
						mtstate->ps.state);

	/* Construct an execution state. */
	fmstate = create_foreign_modify(mtstate->ps.state,
									rte,
									resultRelInfo,
									mtstate->operation,
									mtstate->mt_plans[subplan_index]->plan,
									query,
									target_attrs,
									has_returning,
									retrieved_attrs);

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
	TupleTableSlot *rslot;

	/*
	 * If the fmstate has aux_fmstate set, use the aux_fmstate (see
	 * postgresBeginForeignInsert())
	 */
	if (fmstate->aux_fmstate)
		resultRelInfo->ri_FdwState = fmstate->aux_fmstate;
	rslot = execute_foreign_modify(estate, resultRelInfo, CMD_INSERT,
								   slot, planSlot);
	/* Revert that change */
	if (fmstate->aux_fmstate)
		resultRelInfo->ri_FdwState = fmstate;

	return rslot;
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
	return execute_foreign_modify(estate, resultRelInfo, CMD_UPDATE,
								  slot, planSlot);
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
	return execute_foreign_modify(estate, resultRelInfo, CMD_DELETE,
								  slot, planSlot);
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

	/* Destroy the execution state */
	finish_foreign_modify(fmstate);
}

/*
 * postgresBeginForeignInsert
 *		Begin an insert operation on a foreign table
 */
static void
postgresBeginForeignInsert(ModifyTableState *mtstate,
						   ResultRelInfo *resultRelInfo)
{
	PgFdwModifyState *fmstate;
	ModifyTable *plan = castNode(ModifyTable, mtstate->ps.plan);
	EState	   *estate = mtstate->ps.state;
	Index		resultRelation;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	RangeTblEntry *rte;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			attnum;
	StringInfoData sql;
	List	   *targetAttrs = NIL;
	List	   *retrieved_attrs = NIL;
	bool		doNothing = false;

	/*
	 * If the foreign table we are about to insert routed rows into is also an
	 * UPDATE subplan result rel that will be updated later, proceeding with
	 * the INSERT will result in the later UPDATE incorrectly modifying those
	 * routed rows, so prevent the INSERT --- it would be nice if we could
	 * handle this case; but for now, throw an error for safety.
	 */
	if (plan && plan->operation == CMD_UPDATE &&
		(resultRelInfo->ri_usesFdwDirectModify ||
		 resultRelInfo->ri_FdwState) &&
		resultRelInfo > mtstate->resultRelInfo + mtstate->mt_whichplan)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot route tuples into foreign table to be updated \"%s\"",
						RelationGetRelationName(rel))));

	initStringInfo(&sql);

	/* We transmit all columns that are defined in the foreign table. */
	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (!attr->attisdropped)
			targetAttrs = lappend_int(targetAttrs, attnum);
	}

	/* Check if we add the ON CONFLICT clause to the remote query. */
	if (plan)
	{
		OnConflictAction onConflictAction = plan->onConflictAction;

		/* We only support DO NOTHING without an inference specification. */
		if (onConflictAction == ONCONFLICT_NOTHING)
			doNothing = true;
		else if (onConflictAction != ONCONFLICT_NONE)
			elog(ERROR, "unexpected ON CONFLICT specification: %d",
				 (int) onConflictAction);
	}

	/*
	 * If the foreign table is a partition that doesn't have a corresponding
	 * RTE entry, we need to create a new RTE
	 * describing the foreign table for use by deparseInsertSql and
	 * create_foreign_modify() below, after first copying the parent's RTE and
	 * modifying some fields to describe the foreign partition to work on.
	 * However, if this is invoked by UPDATE, the existing RTE may already
	 * correspond to this partition if it is one of the UPDATE subplan target
	 * rels; in that case, we can just use the existing RTE as-is.
	 */
	if (resultRelInfo->ri_RangeTableIndex == 0)
	{
		ResultRelInfo *rootResultRelInfo = resultRelInfo->ri_RootResultRelInfo;

		rte = exec_rt_fetch(rootResultRelInfo->ri_RangeTableIndex, estate);
		rte = copyObject(rte);
		rte->relid = RelationGetRelid(rel);
		rte->relkind = RELKIND_FOREIGN_TABLE;

		/*
		 * For UPDATE, we must use the RT index of the first subplan target
		 * rel's RTE, because the core code would have built expressions for
		 * the partition, such as RETURNING, using that RT index as varno of
		 * Vars contained in those expressions.
		 */
		if (plan && plan->operation == CMD_UPDATE &&
			rootResultRelInfo->ri_RangeTableIndex == plan->rootRelation)
			resultRelation = mtstate->resultRelInfo[0].ri_RangeTableIndex;
		else
			resultRelation = rootResultRelInfo->ri_RangeTableIndex;
	}
	else
	{
		resultRelation = resultRelInfo->ri_RangeTableIndex;
		rte = exec_rt_fetch(resultRelation, estate);
	}

	/* Construct the SQL command string. */
	deparseInsertSql(&sql, rte, resultRelation, rel, targetAttrs, doNothing,
					 resultRelInfo->ri_WithCheckOptions,
					 resultRelInfo->ri_returningList,
					 &retrieved_attrs);

	/* Construct an execution state. */
	fmstate = create_foreign_modify(mtstate->ps.state,
									rte,
									resultRelInfo,
									CMD_INSERT,
									NULL,
									sql.data,
									targetAttrs,
									retrieved_attrs != NIL,
									retrieved_attrs);

	/*
	 * If the given resultRelInfo already has PgFdwModifyState set, it means
	 * the foreign table is an UPDATE subplan result rel; in which case, store
	 * the resulting state into the aux_fmstate of the PgFdwModifyState.
	 */
	if (resultRelInfo->ri_FdwState)
	{
		Assert(plan && plan->operation == CMD_UPDATE);
		Assert(resultRelInfo->ri_usesFdwDirectModify == false);
		((PgFdwModifyState *) resultRelInfo->ri_FdwState)->aux_fmstate = fmstate;
	}
	else
		resultRelInfo->ri_FdwState = fmstate;
}

/*
 * postgresEndForeignInsert
 *		Finish an insert operation on a foreign table
 */
static void
postgresEndForeignInsert(EState *estate,
						 ResultRelInfo *resultRelInfo)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;

	Assert(fmstate != NULL);

	/*
	 * If the fmstate has aux_fmstate set, get the aux_fmstate (see
	 * postgresBeginForeignInsert())
	 */
	if (fmstate->aux_fmstate)
		fmstate = fmstate->aux_fmstate;

	/* Destroy the execution state */
	finish_foreign_modify(fmstate);
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
 * Decide whether it is safe to modify a foreign table directly, and if so,
 * rewrite subplan accordingly.
 */
static bool
postgresPlanDirectModify(PlannerInfo *root,
						 ModifyTable *plan,
						 Index resultRelation,
						 int subplan_index)
{
	CmdType		operation = plan->operation;
	Plan	   *subplan;
	RelOptInfo *foreignrel;
	RangeTblEntry *rte;
	PgFdwRelationInfo *fpinfo;
	Relation	rel;
	StringInfoData sql;
	ForeignScan *fscan;
	List	   *targetAttrs = NIL;
	List	   *remote_exprs;
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
	subplan = (Plan *) list_nth(plan->plans, subplan_index);
	if (!IsA(subplan, ForeignScan))
		return false;
	fscan = (ForeignScan *) subplan;

	/*
	 * It's unsafe to modify a foreign table directly if there are any quals
	 * that should be evaluated locally.
	 */
	if (subplan->qual != NIL)
		return false;

	/* Safe to fetch data about the target foreign rel */
	if (fscan->scan.scanrelid == 0)
	{
		foreignrel = find_join_rel(root, fscan->fs_relids);
		/* We should have a rel for this foreign join. */
		Assert(foreignrel);
	}
	else
		foreignrel = root->simple_rel_array[resultRelation];
	rte = root->simple_rte_array[resultRelation];
	fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;

	/*
	 * It's unsafe to update a foreign table directly, if any expressions to
	 * assign to the target columns are unsafe to evaluate remotely.
	 */
	if (operation == CMD_UPDATE)
	{
		int			col;

		/*
		 * We transmit only columns that were explicitly targets of the
		 * UPDATE, so as to avoid unnecessary data transmission.
		 */
		col = -1;
		while ((col = bms_next_member(rte->updatedCols, col)) >= 0)
		{
			/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
			AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;
			TargetEntry *tle;

			if (attno <= InvalidAttrNumber) /* shouldn't happen */
				elog(ERROR, "system-column update is not supported");

			tle = get_tle_by_resno(subplan->targetlist, attno);

			if (!tle)
				elog(ERROR, "attribute number %d not found in subplan targetlist",
					 attno);

			if (!is_foreign_expr(root, foreignrel, (Expr *) tle->expr))
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
	rel = table_open(rte->relid, NoLock);

	/*
	 * Recall the qual clauses that must be evaluated remotely.  (These are
	 * bare clauses not RestrictInfos, but deparse.c's appendConditions()
	 * doesn't care.)
	 */
	remote_exprs = fpinfo->final_remote_exprs;

	/*
	 * Extract the relevant RETURNING list if any.
	 */
	if (plan->returningLists)
	{
		returningList = (List *) list_nth(plan->returningLists, subplan_index);

		/*
		 * When performing an UPDATE/DELETE .. RETURNING on a join directly,
		 * we fetch from the foreign server any Vars specified in RETURNING
		 * that refer not only to the target relation but to non-target
		 * relations.  So we'll deparse them into the RETURNING clause of the
		 * remote query; use a targetlist consisting of them instead, which
		 * will be adjusted to be new fdw_scan_tlist of the foreign-scan plan
		 * node below.
		 */
		if (fscan->scan.scanrelid == 0)
			returningList = build_remote_returning(resultRelation, rel,
												   returningList);
	}

	/*
	 * Construct the SQL command string.
	 */
	switch (operation)
	{
		case CMD_UPDATE:
			deparseDirectUpdateSql(&sql, root, resultRelation, rel,
								   foreignrel,
								   ((Plan *) fscan)->targetlist,
								   targetAttrs,
								   remote_exprs, &params_list,
								   returningList, &retrieved_attrs);
			break;
		case CMD_DELETE:
			deparseDirectDeleteSql(&sql, root, resultRelation, rel,
								   foreignrel,
								   remote_exprs, &params_list,
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

	/*
	 * Update the foreign-join-related fields.
	 */
	if (fscan->scan.scanrelid == 0)
	{
		/* No need for the outer subplan. */
		fscan->scan.plan.lefttree = NULL;

		/* Build new fdw_scan_tlist if UPDATE/DELETE .. RETURNING. */
		if (returningList)
			rebuild_fdw_scan_tlist(fscan, returningList);
	}

	table_close(rel, NoLock);
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
	Index		rtindex;
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
	rtindex = estate->es_result_relation_info->ri_RangeTableIndex;
	rte = exec_rt_fetch(rtindex, estate);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	if (fsplan->scan.scanrelid == 0)
		dmstate->rel = ExecOpenScanRelation(estate, rtindex, eflags);
	else
		dmstate->rel = node->ss.ss_currentRelation;
	table = GetForeignTable(RelationGetRelid(dmstate->rel));
	user = GetUserMapping(userid, table->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	dmstate->conn = GetConnection(user, false);

	/* Update the foreign-join-related fields. */
	if (fsplan->scan.scanrelid == 0)
	{
		/* Save info about foreign table. */
		dmstate->resultRel = dmstate->rel;

		/*
		 * Set dmstate->rel to NULL to teach get_returning_data() and
		 * make_tuple_from_result_row() that columns fetched from the remote
		 * server are described by fdw_scan_tlist of the foreign-scan plan
		 * node, not the tuple descriptor for the target relation.
		 */
		dmstate->rel = NULL;
	}

	/* Initialize state variable */
	dmstate->num_tuples = -1;	/* -1 means not set yet */

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
											  ALLOCSET_SMALL_SIZES);

	/* Prepare for input conversion of RETURNING results. */
	if (dmstate->has_returning)
	{
		TupleDesc	tupdesc;

		if (fsplan->scan.scanrelid == 0)
			tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
		else
			tupdesc = RelationGetDescr(dmstate->rel);

		dmstate->attinmeta = TupleDescGetAttInMetadata(tupdesc);

		/*
		 * When performing an UPDATE/DELETE .. RETURNING on a join directly,
		 * initialize a filter to extract an updated/deleted tuple from a scan
		 * tuple.
		 */
		if (fsplan->scan.scanrelid == 0)
			init_returning_filter(dmstate, fsplan->fdw_scan_tlist, rtindex);
	}

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
	ForeignScan *plan = castNode(ForeignScan, node->ss.ps.plan);
	List	   *fdw_private = plan->fdw_private;

	/*
	 * Identify foreign scans that are really joins or upper relations.  The
	 * input looks something like "(1) LEFT JOIN (2)", and we must replace the
	 * digit string(s), which are RT indexes, with the correct relation names.
	 * We do that here, not when the plan is created, because we can't know
	 * what aliases ruleutils.c will assign at plan creation time.
	 */
	if (list_length(fdw_private) > FdwScanPrivateRelations)
	{
		StringInfo	relations;
		char	   *rawrelations;
		char	   *ptr;
		int			minrti,
					rtoffset;

		rawrelations = strVal(list_nth(fdw_private, FdwScanPrivateRelations));

		/*
		 * A difficulty with using a string representation of RT indexes is
		 * that setrefs.c won't update the string when flattening the
		 * rangetable.  To find out what rtoffset was applied, identify the
		 * minimum RT index appearing in the string and compare it to the
		 * minimum member of plan->fs_relids.  (We expect all the relids in
		 * the join will have been offset by the same amount; the Asserts
		 * below should catch it if that ever changes.)
		 */
		minrti = INT_MAX;
		ptr = rawrelations;
		while (*ptr)
		{
			if (isdigit((unsigned char) *ptr))
			{
				int			rti = strtol(ptr, &ptr, 10);

				if (rti < minrti)
					minrti = rti;
			}
			else
				ptr++;
		}
		rtoffset = bms_next_member(plan->fs_relids, -1) - minrti;

		/* Now we can translate the string */
		relations = makeStringInfo();
		ptr = rawrelations;
		while (*ptr)
		{
			if (isdigit((unsigned char) *ptr))
			{
				int			rti = strtol(ptr, &ptr, 10);
				RangeTblEntry *rte;
				char	   *relname;
				char	   *refname;

				rti += rtoffset;
				Assert(bms_is_member(rti, plan->fs_relids));
				rte = rt_fetch(rti, es->rtable);
				Assert(rte->rtekind == RTE_RELATION);
				/* This logic should agree with explain.c's ExplainTargetRel */
				relname = get_rel_name(rte->relid);
				if (es->verbose)
				{
					char	   *namespace;

					namespace = get_namespace_name(get_rel_namespace(rte->relid));
					appendStringInfo(relations, "%s.%s",
									 quote_identifier(namespace),
									 quote_identifier(relname));
				}
				else
					appendStringInfo(relations, "%s",
									 quote_identifier(relname));
				refname = (char *) list_nth(es->rtable_names, rti - 1);
				if (refname == NULL)
					refname = rte->eref->aliasname;
				if (strcmp(refname, relname) != 0)
					appendStringInfo(relations, " %s",
									 quote_identifier(refname));
			}
			else
				appendStringInfoChar(relations, *ptr++);
		}
		ExplainPropertyText("Relations", relations->data, es);
	}

	/*
	 * Add remote query, when VERBOSE option is specified.
	 */
	if (es->verbose)
	{
		char	   *sql;

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
 *		either a base relation or a join between foreign relations or an upper
 *		relation containing foreign relations.
 *
 * param_join_conds are the parameterization clauses with outer relations.
 * pathkeys specify the expected sort order if any for given path being costed.
 * fpextra specifies additional post-scan/join-processing steps such as the
 * final sort and the LIMIT restriction.
 *
 * The function returns the cost and size estimates in p_rows, p_width,
 * p_startup_cost and p_total_cost variables.
 */
static void
estimate_path_cost_size(PlannerInfo *root,
						RelOptInfo *foreignrel,
						List *param_join_conds,
						List *pathkeys,
						PgFdwPathExtraData *fpextra,
						double *p_rows, int *p_width,
						Cost *p_startup_cost, Cost *p_total_cost)
{
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) foreignrel->fdw_private;
	double		rows;
	double		retrieved_rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

	/* Make sure the core code has set up the relation's reltarget */
	Assert(foreignrel->reltarget);

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
		if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
			fdw_scan_tlist = build_tlist_to_deparse(foreignrel);
		else
			fdw_scan_tlist = NIL;

		/*
		 * The complete list of remote conditions includes everything from
		 * baserestrictinfo plus any extra join_conds relevant to this
		 * particular path.
		 */
		remote_conds = list_concat(remote_param_join_conds,
								   fpinfo->remote_conds);

		/*
		 * Construct EXPLAIN query including the desired SELECT, FROM, and
		 * WHERE clauses. Params and other-relation Vars are replaced by dummy
		 * values, so don't request params_list.
		 */
		initStringInfo(&sql);
		appendStringInfoString(&sql, "EXPLAIN ");
		deparseSelectStmtForRel(&sql, root, foreignrel, fdw_scan_tlist,
								remote_conds, pathkeys,
								fpextra ? fpextra->has_final_sort : false,
								fpextra ? fpextra->has_limit : false,
								false, &retrieved_attrs, NULL);

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

		/*
		 * Add in tlist eval cost for each output row.  In case of an
		 * aggregate, some of the tlist expressions such as grouping
		 * expressions will be evaluated remotely, so adjust the costs.
		 */
		startup_cost += foreignrel->reltarget->cost.startup;
		total_cost += foreignrel->reltarget->cost.startup;
		total_cost += foreignrel->reltarget->cost.per_tuple * rows;
		if (IS_UPPER_REL(foreignrel))
		{
			QualCost	tlist_cost;

			cost_qual_eval(&tlist_cost, fdw_scan_tlist, root);
			startup_cost -= tlist_cost.startup;
			total_cost -= tlist_cost.startup;
			total_cost -= tlist_cost.per_tuple * rows;
		}
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
		 * We will come here again and again with different set of pathkeys or
		 * additional post-scan/join-processing steps that caller wants to
		 * cost.  We don't need to calculate the cost/size estimates for the
		 * underlying scan, join, or grouping each time.  Instead, use those
		 * estimates if we have cached them already.
		 */
		if (fpinfo->rel_startup_cost >= 0 && fpinfo->rel_total_cost >= 0)
		{
			Assert(fpinfo->retrieved_rows >= 0);

			rows = fpinfo->rows;
			retrieved_rows = fpinfo->retrieved_rows;
			width = fpinfo->width;
			startup_cost = fpinfo->rel_startup_cost;
			run_cost = fpinfo->rel_total_cost - fpinfo->rel_startup_cost;

			/*
			 * If we estimate the costs of a foreign scan or a foreign join
			 * with additional post-scan/join-processing steps, the scan or
			 * join costs obtained from the cache wouldn't yet contain the
			 * eval costs for the final scan/join target, which would've been
			 * updated by apply_scanjoin_target_to_paths(); add the eval costs
			 * now.
			 */
			if (fpextra && !IS_UPPER_REL(foreignrel))
			{
				/* Shouldn't get here unless we have LIMIT */
				Assert(fpextra->has_limit);
				Assert(foreignrel->reloptkind == RELOPT_BASEREL ||
					   foreignrel->reloptkind == RELOPT_JOINREL);
				startup_cost += foreignrel->reltarget->cost.startup;
				run_cost += foreignrel->reltarget->cost.per_tuple * rows;
			}
		}
		else if (IS_JOIN_REL(foreignrel))
		{
			PgFdwRelationInfo *fpinfo_i;
			PgFdwRelationInfo *fpinfo_o;
			QualCost	join_cost;
			QualCost	remote_conds_cost;
			double		nrows;

			/* Use rows/width estimates made by the core code. */
			rows = foreignrel->rows;
			width = foreignrel->reltarget->width;

			/* For join we expect inner and outer relations set */
			Assert(fpinfo->innerrel && fpinfo->outerrel);

			fpinfo_i = (PgFdwRelationInfo *) fpinfo->innerrel->fdw_private;
			fpinfo_o = (PgFdwRelationInfo *) fpinfo->outerrel->fdw_private;

			/* Estimate of number of rows in cross product */
			nrows = fpinfo_i->rows * fpinfo_o->rows;

			/*
			 * Back into an estimate of the number of retrieved rows.  Just in
			 * case this is nuts, clamp to at most nrows.
			 */
			retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);
			retrieved_rows = Min(retrieved_rows, nrows);

			/*
			 * The cost of foreign join is estimated as cost of generating
			 * rows for the joining relations + cost for applying quals on the
			 * rows.
			 */

			/*
			 * Calculate the cost of clauses pushed down to the foreign server
			 */
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

			/* Add in tlist eval cost for each output row */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
		}
		else if (IS_UPPER_REL(foreignrel))
		{
			RelOptInfo *outerrel = fpinfo->outerrel;
			PgFdwRelationInfo *ofpinfo;
			AggClauseCosts aggcosts;
			double		input_rows;
			int			numGroupCols;
			double		numGroups = 1;

			/* The upper relation should have its outer relation set */
			Assert(outerrel);
			/* and that outer relation should have its reltarget set */
			Assert(outerrel->reltarget);

			/*
			 * This cost model is mixture of costing done for sorted and
			 * hashed aggregates in cost_agg().  We are not sure which
			 * strategy will be considered at remote side, thus for
			 * simplicity, we put all startup related costs in startup_cost
			 * and all finalization and run cost are added in total_cost.
			 */

			ofpinfo = (PgFdwRelationInfo *) outerrel->fdw_private;

			/* Get rows from input rel */
			input_rows = ofpinfo->rows;

			/* Collect statistics about aggregates for estimating costs. */
			MemSet(&aggcosts, 0, sizeof(AggClauseCosts));
			if (root->parse->hasAggs)
			{
				get_agg_clause_costs(root, (Node *) fpinfo->grouped_tlist,
									 AGGSPLIT_SIMPLE, &aggcosts);

				/*
				 * The cost of aggregates in the HAVING qual will be the same
				 * for each child as it is for the parent, so there's no need
				 * to use a translated version of havingQual.
				 */
				get_agg_clause_costs(root, (Node *) root->parse->havingQual,
									 AGGSPLIT_SIMPLE, &aggcosts);
			}

			/* Get number of grouping columns and possible number of groups */
			numGroupCols = list_length(root->parse->groupClause);
			numGroups = estimate_num_groups(root,
											get_sortgrouplist_exprs(root->parse->groupClause,
																	fpinfo->grouped_tlist),
											input_rows, NULL);

			/*
			 * Get the retrieved_rows and rows estimates.  If there are HAVING
			 * quals, account for their selectivity.
			 */
			if (root->parse->havingQual)
			{
				/* Factor in the selectivity of the remotely-checked quals */
				retrieved_rows =
					clamp_row_est(numGroups *
								  clauselist_selectivity(root,
														 fpinfo->remote_conds,
														 0,
														 JOIN_INNER,
														 NULL));
				/* Factor in the selectivity of the locally-checked quals */
				rows = clamp_row_est(retrieved_rows * fpinfo->local_conds_sel);
			}
			else
			{
				rows = retrieved_rows = numGroups;
			}

			/* Use width estimate made by the core code. */
			width = foreignrel->reltarget->width;

			/*-----
			 * Startup cost includes:
			 *	  1. Startup cost for underneath input relation, adjusted for
			 *	     tlist replacement by apply_scanjoin_target_to_paths()
			 *	  2. Cost of performing aggregation, per cost_agg()
			 *-----
			 */
			startup_cost = ofpinfo->rel_startup_cost;
			startup_cost += outerrel->reltarget->cost.startup;
			startup_cost += aggcosts.transCost.startup;
			startup_cost += aggcosts.transCost.per_tuple * input_rows;
			startup_cost += aggcosts.finalCost.startup;
			startup_cost += (cpu_operator_cost * numGroupCols) * input_rows;

			/*-----
			 * Run time cost includes:
			 *	  1. Run time cost of underneath input relation, adjusted for
			 *	     tlist replacement by apply_scanjoin_target_to_paths()
			 *	  2. Run time cost of performing aggregation, per cost_agg()
			 *-----
			 */
			run_cost = ofpinfo->rel_total_cost - ofpinfo->rel_startup_cost;
			run_cost += outerrel->reltarget->cost.per_tuple * input_rows;
			run_cost += aggcosts.finalCost.per_tuple * numGroups;
			run_cost += cpu_tuple_cost * numGroups;

			/* Account for the eval cost of HAVING quals, if any */
			if (root->parse->havingQual)
			{
				QualCost	remote_cost;

				/* Add in the eval cost of the remotely-checked quals */
				cost_qual_eval(&remote_cost, fpinfo->remote_conds, root);
				startup_cost += remote_cost.startup;
				run_cost += remote_cost.per_tuple * numGroups;
				/* Add in the eval cost of the locally-checked quals */
				startup_cost += fpinfo->local_conds_cost.startup;
				run_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
			}

			/* Add in tlist eval cost for each output row */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
		}
		else
		{
			Cost		cpu_per_tuple;

			/* Use rows/width estimates made by set_baserel_size_estimates. */
			rows = foreignrel->rows;
			width = foreignrel->reltarget->width;

			/*
			 * Back into an estimate of the number of retrieved rows.  Just in
			 * case this is nuts, clamp to at most foreignrel->tuples.
			 */
			retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);
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

			/* Add in tlist eval cost for each output row */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
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
			if (IS_UPPER_REL(foreignrel))
			{
				Assert(foreignrel->reloptkind == RELOPT_UPPER_REL &&
					   fpinfo->stage == UPPERREL_GROUP_AGG);
				adjust_foreign_grouping_path_cost(root, pathkeys,
												  retrieved_rows, width,
												  fpextra->limit_tuples,
												  &startup_cost, &run_cost);
			}
			else
			{
				startup_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
				run_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
			}
		}

		total_cost = startup_cost + run_cost;

		/* Adjust the cost estimates if we have LIMIT */
		if (fpextra && fpextra->has_limit)
		{
			adjust_limit_rows_costs(&rows, &startup_cost, &total_cost,
									fpextra->offset_est, fpextra->count_est);
			retrieved_rows = rows;
		}
	}

	/*
	 * If this includes the final sort step, the given target, which will be
	 * applied to the resulting path, might have different expressions from
	 * the foreignrel's reltarget (see make_sort_input_target()); adjust tlist
	 * eval costs.
	 */
	if (fpextra && fpextra->has_final_sort &&
		fpextra->target != foreignrel->reltarget)
	{
		QualCost	oldcost = foreignrel->reltarget->cost;
		QualCost	newcost = fpextra->target->cost;

		startup_cost += newcost.startup - oldcost.startup;
		total_cost += newcost.startup - oldcost.startup;
		total_cost += (newcost.per_tuple - oldcost.per_tuple) * rows;
	}

	/*
	 * Cache the retrieved rows and cost estimates for scans, joins, or
	 * groupings without any parameterization, pathkeys, or additional
	 * post-scan/join-processing steps, before adding the costs for
	 * transferring data from the foreign server.  These estimates are useful
	 * for costing remote joins involving this relation or costing other
	 * remote operations on this relation such as remote sorts and remote
	 * LIMIT restrictions, when the costs can not be obtained from the foreign
	 * server.  This function will be called at least once for every foreign
	 * relation without any parameterization, pathkeys, or additional
	 * post-scan/join-processing steps.
	 */
	if (pathkeys == NIL && param_join_conds == NIL && fpextra == NULL)
	{
		fpinfo->retrieved_rows = retrieved_rows;
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

	/*
	 * If we have LIMIT, we should prefer performing the restriction remotely
	 * rather than locally, as the former avoids extra row fetches from the
	 * remote that the latter might cause.  But since the core code doesn't
	 * account for such fetches when estimating the costs of the local
	 * restriction (see create_limit_path()), there would be no difference
	 * between the costs of the local restriction and the costs of the remote
	 * restriction estimated above if we don't use remote estimates (except
	 * for the case where the foreignrel is a grouping relation, the given
	 * pathkeys is not NIL, and the effects of a bounded sort for that rel is
	 * accounted for in costing the remote restriction).  Tweak the costs of
	 * the remote restriction to ensure we'll prefer it if LIMIT is a useful
	 * one.
	 */
	if (!fpinfo->use_remote_estimate &&
		fpextra && fpextra->has_limit &&
		fpextra->limit_tuples > 0 &&
		fpextra->limit_tuples < fpinfo->rows)
	{
		Assert(fpinfo->rows > 0);
		total_cost -= (total_cost - startup_cost) * 0.05 *
			(fpinfo->rows - fpextra->limit_tuples) / fpinfo->rows;
	}

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
		res = pgfdw_exec_query(conn, sql);
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
	}
	PG_FINALLY();
	{
		if (res)
			PQclear(res);
	}
	PG_END_TRY();
}

/*
 * Adjust the cost estimates of a foreign grouping path to include the cost of
 * generating properly-sorted output.
 */
static void
adjust_foreign_grouping_path_cost(PlannerInfo *root,
								  List *pathkeys,
								  double retrieved_rows,
								  double width,
								  double limit_tuples,
								  Cost *p_startup_cost,
								  Cost *p_run_cost)
{
	/*
	 * If the GROUP BY clause isn't sort-able, the plan chosen by the remote
	 * side is unlikely to generate properly-sorted output, so it would need
	 * an explicit sort; adjust the given costs with cost_sort().  Likewise,
	 * if the GROUP BY clause is sort-able but isn't a superset of the given
	 * pathkeys, adjust the costs with that function.  Otherwise, adjust the
	 * costs by applying the same heuristic as for the scan or join case.
	 */
	if (!grouping_is_sortable(root->parse->groupClause) ||
		!pathkeys_contained_in(pathkeys, root->group_pathkeys))
	{
		Path		sort_path;	/* dummy for result of cost_sort */

		cost_sort(&sort_path,
				  root,
				  pathkeys,
				  *p_startup_cost + *p_run_cost,
				  retrieved_rows,
				  width,
				  0.0,
				  work_mem,
				  limit_tuples);

		*p_startup_cost = sort_path.startup_cost;
		*p_run_cost = sort_path.total_cost - sort_path.startup_cost;
	}
	else
	{
		/*
		 * The default extra cost seems too large for foreign-grouping cases;
		 * add 1/4th of that default.
		 */
		double		sort_multiplier = 1.0 + (DEFAULT_FDW_SORT_MULTIPLIER
											 - 1.0) * 0.25;

		*p_startup_cost *= sort_multiplier;
		*p_run_cost *= sort_multiplier;
	}
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
	 */
	if (!PQsendQueryParams(conn, buf.data, numParams,
						   NULL, values, NULL, NULL, 0))
		pgfdw_report_error(ERROR, NULL, conn, false, buf.data);

	/*
	 * Get the result, and check for success.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = pgfdw_get_result(conn, buf.data);
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

		res = pgfdw_exec_query(conn, sql);
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
	}
	PG_FINALLY();
	{
		if (res)
			PQclear(res);
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

	/*
	 * In addition force restrictive search_path, in case there are any
	 * regproc or similar constants to be printed.
	 */
	(void) set_config_option("search_path", "pg_catalog",
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
	res = pgfdw_exec_query(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	PQclear(res);
}

/*
 * create_foreign_modify
 *		Construct an execution state of a foreign insert/update/delete
 *		operation
 */
static PgFdwModifyState *
create_foreign_modify(EState *estate,
					  RangeTblEntry *rte,
					  ResultRelInfo *resultRelInfo,
					  CmdType operation,
					  Plan *subplan,
					  char *query,
					  List *target_attrs,
					  bool has_returning,
					  List *retrieved_attrs)
{
	PgFdwModifyState *fmstate;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	AttrNumber	n_params;
	Oid			typefnoid;
	bool		isvarlena;
	ListCell   *lc;

	/* Begin constructing PgFdwModifyState. */
	fmstate = (PgFdwModifyState *) palloc0(sizeof(PgFdwModifyState));
	fmstate->rel = rel;

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does.
	 */
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	table = GetForeignTable(RelationGetRelid(rel));
	user = GetUserMapping(userid, table->serverid);

	/* Open connection; report that we'll create a prepared statement. */
	fmstate->conn = GetConnection(user, true);
	fmstate->p_name = NULL;		/* prepared statement not made yet */

	/* Set up remote query information. */
	fmstate->query = query;
	fmstate->target_attrs = target_attrs;
	fmstate->has_returning = has_returning;
	fmstate->retrieved_attrs = retrieved_attrs;

	/* Create context for per-tuple temp workspace. */
	fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "postgres_fdw temporary data",
											  ALLOCSET_SMALL_SIZES);

	/* Prepare for input conversion of RETURNING results. */
	if (fmstate->has_returning)
		fmstate->attinmeta = TupleDescGetAttInMetadata(tupdesc);

	/* Prepare for output conversion of parameters used in prepared stmt. */
	n_params = list_length(fmstate->target_attrs) + 1;
	fmstate->p_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * n_params);
	fmstate->p_nums = 0;

	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		Assert(subplan != NULL);

		/* Find the ctid resjunk column in the subplan's result */
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
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			Assert(!attr->attisdropped);

			/* Ignore generated columns; they are set to DEFAULT */
			if (attr->attgenerated)
				continue;
			getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
			fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
			fmstate->p_nums++;
		}
	}

	Assert(fmstate->p_nums <= n_params);

	/* Initialize auxiliary state */
	fmstate->aux_fmstate = NULL;

	return fmstate;
}

/*
 * execute_foreign_modify
 *		Perform foreign-table modification as required, and fetch RETURNING
 *		result if any.  (This is the shared guts of postgresExecForeignInsert,
 *		postgresExecForeignUpdate, and postgresExecForeignDelete.)
 */
static TupleTableSlot *
execute_foreign_modify(EState *estate,
					   ResultRelInfo *resultRelInfo,
					   CmdType operation,
					   TupleTableSlot *slot,
					   TupleTableSlot *planSlot)
{
	PgFdwModifyState *fmstate = (PgFdwModifyState *) resultRelInfo->ri_FdwState;
	ItemPointer ctid = NULL;
	const char **p_values;
	PGresult   *res;
	int			n_rows;

	/* The operation should be INSERT, UPDATE, or DELETE */
	Assert(operation == CMD_INSERT ||
		   operation == CMD_UPDATE ||
		   operation == CMD_DELETE);

	/* Set up the prepared statement on the remote server, if we didn't yet */
	if (!fmstate->p_name)
		prepare_foreign_modify(fmstate);

	/*
	 * For UPDATE/DELETE, get the ctid that was passed up as a resjunk column
	 */
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		Datum		datum;
		bool		isNull;

		datum = ExecGetJunkAttribute(planSlot,
									 fmstate->ctidAttno,
									 &isNull);
		/* shouldn't ever get a null result... */
		if (isNull)
			elog(ERROR, "ctid is NULL");
		ctid = (ItemPointer) DatumGetPointer(datum);
	}

	/* Convert parameters needed by prepared statement to text form */
	p_values = convert_prep_stmt_params(fmstate, ctid, slot);

	/*
	 * Execute the prepared statement.
	 */
	if (!PQsendQueryPrepared(fmstate->conn,
							 fmstate->p_name,
							 fmstate->p_nums,
							 p_values,
							 NULL,
							 NULL,
							 0))
		pgfdw_report_error(ERROR, NULL, fmstate->conn, false, fmstate->query);

	/*
	 * Get the result, and check for success.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = pgfdw_get_result(fmstate->conn, fmstate->query);
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

	/*
	 * Return NULL if nothing was inserted/updated/deleted on the remote end
	 */
	return (n_rows > 0) ? slot : NULL;
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
	 */
	if (!PQsendPrepare(fmstate->conn,
					   p_name,
					   fmstate->query,
					   0,
					   NULL))
		pgfdw_report_error(ERROR, NULL, fmstate->conn, false, fmstate->query);

	/*
	 * Get the result, and check for success.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	res = pgfdw_get_result(fmstate->conn, fmstate->query);
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
		TupleDesc	tupdesc = RelationGetDescr(fmstate->rel);
		int			nestlevel;
		ListCell   *lc;

		nestlevel = set_transmission_modes();

		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
			Datum		value;
			bool		isnull;

			/* Ignore generated columns; they are set to DEFAULT */
			if (attr->attgenerated)
				continue;
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

		/*
		 * The returning slot will not necessarily be suitable to store
		 * heaptuples directly, so allow for conversion.
		 */
		ExecForceStoreHeapTuple(newtup, slot, true);
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
 * finish_foreign_modify
 *		Release resources for a foreign insert/update/delete operation
 */
static void
finish_foreign_modify(PgFdwModifyState *fmstate)
{
	Assert(fmstate != NULL);

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
		res = pgfdw_exec_query(fmstate->conn, sql);
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
 * build_remote_returning
 *		Build a RETURNING targetlist of a remote query for performing an
 *		UPDATE/DELETE .. RETURNING on a join directly
 */
static List *
build_remote_returning(Index rtindex, Relation rel, List *returningList)
{
	bool		have_wholerow = false;
	List	   *tlist = NIL;
	List	   *vars;
	ListCell   *lc;

	Assert(returningList);

	vars = pull_var_clause((Node *) returningList, PVC_INCLUDE_PLACEHOLDERS);

	/*
	 * If there's a whole-row reference to the target relation, then we'll
	 * need all the columns of the relation.
	 */
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		if (IsA(var, Var) &&
			var->varno == rtindex &&
			var->varattno == InvalidAttrNumber)
		{
			have_wholerow = true;
			break;
		}
	}

	if (have_wholerow)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			i;

		for (i = 1; i <= tupdesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);
			Var		   *var;

			/* Ignore dropped attributes. */
			if (attr->attisdropped)
				continue;

			var = makeVar(rtindex,
						  i,
						  attr->atttypid,
						  attr->atttypmod,
						  attr->attcollation,
						  0);

			tlist = lappend(tlist,
							makeTargetEntry((Expr *) var,
											list_length(tlist) + 1,
											NULL,
											false));
		}
	}

	/* Now add any remaining columns to tlist. */
	foreach(lc, vars)
	{
		Var		   *var = (Var *) lfirst(lc);

		/*
		 * No need for whole-row references to the target relation.  We don't
		 * need system columns other than ctid and oid either, since those are
		 * set locally.
		 */
		if (IsA(var, Var) &&
			var->varno == rtindex &&
			var->varattno <= InvalidAttrNumber &&
			var->varattno != SelfItemPointerAttributeNumber)
			continue;			/* don't need it */

		if (tlist_member((Expr *) var, tlist))
			continue;			/* already got it */

		tlist = lappend(tlist,
						makeTargetEntry((Expr *) var,
										list_length(tlist) + 1,
										NULL,
										false));
	}

	list_free(vars);

	return tlist;
}

/*
 * rebuild_fdw_scan_tlist
 *		Build new fdw_scan_tlist of given foreign-scan plan node from given
 *		tlist
 *
 * There might be columns that the fdw_scan_tlist of the given foreign-scan
 * plan node contains that the given tlist doesn't.  The fdw_scan_tlist would
 * have contained resjunk columns such as 'ctid' of the target relation and
 * 'wholerow' of non-target relations, but the tlist might not contain them,
 * for example.  So, adjust the tlist so it contains all the columns specified
 * in the fdw_scan_tlist; else setrefs.c will get confused.
 */
static void
rebuild_fdw_scan_tlist(ForeignScan *fscan, List *tlist)
{
	List	   *new_tlist = tlist;
	List	   *old_tlist = fscan->fdw_scan_tlist;
	ListCell   *lc;

	foreach(lc, old_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tlist_member(tle->expr, new_tlist))
			continue;			/* already got it */

		new_tlist = lappend(new_tlist,
							makeTargetEntry(tle->expr,
											list_length(new_tlist) + 1,
											NULL,
											false));
	}
	fscan->fdw_scan_tlist = new_tlist;
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
	 */
	if (!PQsendQueryParams(dmstate->conn, dmstate->query, numParams,
						   NULL, values, NULL, NULL, 0))
		pgfdw_report_error(ERROR, NULL, dmstate->conn, false, dmstate->query);

	/*
	 * Get the result, and check for success.
	 *
	 * We don't use a PG_TRY block here, so be careful not to throw error
	 * without releasing the PGresult.
	 */
	dmstate->result = pgfdw_get_result(dmstate->conn, dmstate->query);
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
	TupleTableSlot *resultSlot;

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
	{
		ExecStoreAllNullTuple(slot);
		resultSlot = slot;
	}
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
												node,
												dmstate->temp_cxt);
			ExecStoreHeapTuple(newtup, slot, false);
		}
		PG_CATCH();
		{
			if (dmstate->result)
				PQclear(dmstate->result);
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* Get the updated/deleted tuple. */
		if (dmstate->rel)
			resultSlot = slot;
		else
			resultSlot = apply_returning_filter(dmstate, slot, estate);
	}
	dmstate->next_tuple++;

	/* Make slot available for evaluation of the local query RETURNING list. */
	resultRelInfo->ri_projectReturning->pi_exprContext->ecxt_scantuple =
		resultSlot;

	return slot;
}

/*
 * Initialize a filter to extract an updated/deleted tuple from a scan tuple.
 */
static void
init_returning_filter(PgFdwDirectModifyState *dmstate,
					  List *fdw_scan_tlist,
					  Index rtindex)
{
	TupleDesc	resultTupType = RelationGetDescr(dmstate->resultRel);
	ListCell   *lc;
	int			i;

	/*
	 * Calculate the mapping between the fdw_scan_tlist's entries and the
	 * result tuple's attributes.
	 *
	 * The "map" is an array of indexes of the result tuple's attributes in
	 * fdw_scan_tlist, i.e., one entry for every attribute of the result
	 * tuple.  We store zero for any attributes that don't have the
	 * corresponding entries in that list, marking that a NULL is needed in
	 * the result tuple.
	 *
	 * Also get the indexes of the entries for ctid and oid if any.
	 */
	dmstate->attnoMap = (AttrNumber *)
		palloc0(resultTupType->natts * sizeof(AttrNumber));

	dmstate->ctidAttno = dmstate->oidAttno = 0;

	i = 1;
	dmstate->hasSystemCols = false;
	foreach(lc, fdw_scan_tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Var		   *var = (Var *) tle->expr;

		Assert(IsA(var, Var));

		/*
		 * If the Var is a column of the target relation to be retrieved from
		 * the foreign server, get the index of the entry.
		 */
		if (var->varno == rtindex &&
			list_member_int(dmstate->retrieved_attrs, i))
		{
			int			attrno = var->varattno;

			if (attrno < 0)
			{
				/*
				 * We don't retrieve system columns other than ctid and oid.
				 */
				if (attrno == SelfItemPointerAttributeNumber)
					dmstate->ctidAttno = i;
				else
					Assert(false);
				dmstate->hasSystemCols = true;
			}
			else
			{
				/*
				 * We don't retrieve whole-row references to the target
				 * relation either.
				 */
				Assert(attrno > 0);

				dmstate->attnoMap[attrno - 1] = i;
			}
		}
		i++;
	}
}

/*
 * Extract and return an updated/deleted tuple from a scan tuple.
 */
static TupleTableSlot *
apply_returning_filter(PgFdwDirectModifyState *dmstate,
					   TupleTableSlot *slot,
					   EState *estate)
{
	ResultRelInfo *relInfo = estate->es_result_relation_info;
	TupleDesc	resultTupType = RelationGetDescr(dmstate->resultRel);
	TupleTableSlot *resultSlot;
	Datum	   *values;
	bool	   *isnull;
	Datum	   *old_values;
	bool	   *old_isnull;
	int			i;

	/*
	 * Use the return tuple slot as a place to store the result tuple.
	 */
	resultSlot = ExecGetReturningSlot(estate, relInfo);

	/*
	 * Extract all the values of the scan tuple.
	 */
	slot_getallattrs(slot);
	old_values = slot->tts_values;
	old_isnull = slot->tts_isnull;

	/*
	 * Prepare to build the result tuple.
	 */
	ExecClearTuple(resultSlot);
	values = resultSlot->tts_values;
	isnull = resultSlot->tts_isnull;

	/*
	 * Transpose data into proper fields of the result tuple.
	 */
	for (i = 0; i < resultTupType->natts; i++)
	{
		int			j = dmstate->attnoMap[i];

		if (j == 0)
		{
			values[i] = (Datum) 0;
			isnull[i] = true;
		}
		else
		{
			values[i] = old_values[j - 1];
			isnull[i] = old_isnull[j - 1];
		}
	}

	/*
	 * Build the virtual tuple.
	 */
	ExecStoreVirtualTuple(resultSlot);

	/*
	 * If we have any system columns to return, materialize a heap tuple in
	 * the slot from column values set above and install system columns in
	 * that tuple.
	 */
	if (dmstate->hasSystemCols)
	{
		HeapTuple	resultTup = ExecFetchSlotHeapTuple(resultSlot, true, NULL);

		/* ctid */
		if (dmstate->ctidAttno)
		{
			ItemPointer ctid = NULL;

			ctid = (ItemPointer) DatumGetPointer(old_values[dmstate->ctidAttno - 1]);
			resultTup->t_self = *ctid;
		}

		/*
		 * And remaining columns
		 *
		 * Note: since we currently don't allow the target relation to appear
		 * on the nullable side of an outer join, any system columns wouldn't
		 * go to NULL.
		 *
		 * Note: no need to care about tableoid here because it will be
		 * initialized in ExecProcessReturning().
		 */
		HeapTupleHeaderSetXmin(resultTup->t_data, InvalidTransactionId);
		HeapTupleHeaderSetXmax(resultTup->t_data, InvalidTransactionId);
		HeapTupleHeaderSetCmin(resultTup->t_data, InvalidTransactionId);
	}

	/*
	 * And return the result tuple.
	 */
	return resultSlot;
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
	*param_exprs = ExecInitExprList(fdw_exprs, node);

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
		expr_value = ExecEvalExpr(expr_state, econtext, &isNull);

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
		res = pgfdw_exec_query(conn, sql.data);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conn, false, sql.data);

		if (PQntuples(res) != 1 || PQnfields(res) != 1)
			elog(ERROR, "unexpected result from deparseAnalyzeSizeSql query");
		*totalpages = strtoul(PQgetvalue(res, 0, 0), NULL, 10);
	}
	PG_FINALLY();
	{
		if (res)
			PQclear(res);
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
											ALLOCSET_SMALL_SIZES);

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
		char		fetch_sql[64];
		int			fetch_size;
		ListCell   *lc;

		res = pgfdw_exec_query(conn, sql.data);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			pgfdw_report_error(ERROR, res, conn, false, sql.data);
		PQclear(res);
		res = NULL;

		/*
		 * Determine the fetch size.  The default is arbitrary, but shouldn't
		 * be enormous.
		 */
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

		/* Construct command to fetch rows from remote. */
		snprintf(fetch_sql, sizeof(fetch_sql), "FETCH %d FROM c%u",
				 fetch_size, cursor_number);

		/* Retrieve and process rows a batch at a time. */
		for (;;)
		{
			int			numrows;
			int			i;

			/* Allow users to cancel long query */
			CHECK_FOR_INTERRUPTS();

			/*
			 * XXX possible future improvement: if rowstoskip is large, we
			 * could issue a MOVE rather than physically fetching the rows,
			 * then just adjust rowstoskip and samplerows appropriately.
			 */

			/* Fetch some rows */
			res = pgfdw_exec_query(conn, fetch_sql);
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
	bool		import_generated = true;
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
		else if (strcmp(def->defname, "import_generated") == 0)
			import_generated = defGetBoolean(def);
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

		res = pgfdw_exec_query(conn, buf.data);
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
		 * Ignore table data for partitions and only include the definitions
		 * of the root partitioned tables to allow access to the complete
		 * remote data set locally in the schema imported.
		 *
		 * Note: because we run the connection with search_path restricted to
		 * pg_catalog, the format_type() and pg_get_expr() outputs will always
		 * include a schema name for types/functions in other schemas, which
		 * is what we want.
		 */
		appendStringInfoString(&buf,
							   "SELECT relname, "
							   "  attname, "
							   "  format_type(atttypid, atttypmod), "
							   "  attnotnull, ");

		/* Generated columns are supported since Postgres 12 */
		if (PQserverVersion(conn) >= 120000)
			appendStringInfoString(&buf,
								   "  attgenerated, "
								   "  pg_get_expr(adbin, adrelid), ");
		else
			appendStringInfoString(&buf,
								   "  NULL, "
								   "  pg_get_expr(adbin, adrelid), ");

		if (import_collate)
			appendStringInfoString(&buf,
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
							   "WHERE c.relkind IN ("
							   CppAsString2(RELKIND_RELATION) ","
							   CppAsString2(RELKIND_VIEW) ","
							   CppAsString2(RELKIND_FOREIGN_TABLE) ","
							   CppAsString2(RELKIND_MATVIEW) ","
							   CppAsString2(RELKIND_PARTITIONED_TABLE) ") "
							   "  AND n.nspname = ");
		deparseStringLiteral(&buf, stmt->remote_schema);

		/* Partitions are supported since Postgres 10 */
		if (PQserverVersion(conn) >= 100000)
			appendStringInfoString(&buf, " AND NOT c.relispartition ");

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
		res = pgfdw_exec_query(conn, buf.data);
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
				char	   *attgenerated;
				char	   *attdefault;
				char	   *collname;
				char	   *collnamespace;

				/* If table has no columns, we'll see nulls here */
				if (PQgetisnull(res, i, 1))
					continue;

				attname = PQgetvalue(res, i, 1);
				typename = PQgetvalue(res, i, 2);
				attnotnull = PQgetvalue(res, i, 3);
				attgenerated = PQgetisnull(res, i, 4) ? (char *) NULL :
					PQgetvalue(res, i, 4);
				attdefault = PQgetisnull(res, i, 5) ? (char *) NULL :
					PQgetvalue(res, i, 5);
				collname = PQgetisnull(res, i, 6) ? (char *) NULL :
					PQgetvalue(res, i, 6);
				collnamespace = PQgetisnull(res, i, 7) ? (char *) NULL :
					PQgetvalue(res, i, 7);

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
				if (import_default && attdefault != NULL &&
					(!attgenerated || !attgenerated[0]))
					appendStringInfo(&buf, " DEFAULT %s", attdefault);

				/* Add GENERATED if needed */
				if (import_generated && attgenerated != NULL &&
					attgenerated[0] == ATTRIBUTE_GENERATED_STORED)
				{
					Assert(attdefault != NULL);
					appendStringInfo(&buf,
									 " GENERATED ALWAYS AS (%s) STORED",
									 attdefault);
				}

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
	}
	PG_FINALLY();
	{
		if (res)
			PQclear(res);
	}
	PG_END_TRY();

	ReleaseConnection(conn);

	return commands;
}

/*
 * Assess whether the join between inner and outer relations can be pushed down
 * to the foreign server. As a side effect, save information we obtain in this
 * function to PgFdwRelationInfo passed in.
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

	/*
	 * Merge FDW options.  We might be tempted to do this after we have deemed
	 * the foreign join to be OK.  But we must do this beforehand so that we
	 * know which quals can be evaluated on the foreign server, which might
	 * depend on shippable_extensions.
	 */
	fpinfo->server = fpinfo_o->server;
	merge_fdw_options(fpinfo, fpinfo_o, fpinfo_i);

	/*
	 * Separate restrict list into join quals and pushed-down (other) quals.
	 *
	 * Join quals belonging to an outer join must all be shippable, else we
	 * cannot execute the join remotely.  Add such quals to 'joinclauses'.
	 *
	 * Add other quals to fpinfo->remote_conds if they are shippable, else to
	 * fpinfo->local_conds.  In an inner join it's okay to execute conditions
	 * either locally or remotely; the same is true for pushed-down conditions
	 * at an outer join.
	 *
	 * Note we might return failure after having already scribbled on
	 * fpinfo->remote_conds and fpinfo->local_conds.  That's okay because we
	 * won't consult those lists again if we deem the join unshippable.
	 */
	joinclauses = NIL;
	foreach(lc, extra->restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		bool		is_remote_clause = is_foreign_expr(root, joinrel,
													   rinfo->clause);

		if (IS_OUTER_JOIN(jointype) &&
			!RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
		{
			if (!is_remote_clause)
				return false;
			joinclauses = lappend(joinclauses, rinfo);
		}
		else
		{
			if (is_remote_clause)
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
			else
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
		}
	}

	/*
	 * deparseExplicitTargetList() isn't smart enough to handle anything other
	 * than a Var.  In particular, if there's some PlaceHolderVar that would
	 * need to be evaluated within this join tree (because there's an upper
	 * reference to a quantity that may go to NULL as a result of an outer
	 * join), then we can't try to push the join down because we'll fail when
	 * we get to deparseExplicitTargetList().  However, a PlaceHolderVar that
	 * needs to be evaluated *at the top* of this join tree is OK, because we
	 * can do that locally after fetching the results from the remote side.
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = lfirst(lc);
		Relids		relids;

		/* PlaceHolderInfo refers to parent relids, not child relids. */
		relids = IS_OTHER_REL(joinrel) ?
			joinrel->top_parent_relids : joinrel->relids;

		if (bms_is_subset(phinfo->ph_eval_at, relids) &&
			bms_nonempty_difference(relids, phinfo->ph_eval_at))
			return false;
	}

	/* Save the join clauses, for later use. */
	fpinfo->joinclauses = joinclauses;

	fpinfo->outerrel = outerrel;
	fpinfo->innerrel = innerrel;
	fpinfo->jointype = jointype;

	/*
	 * By default, both the input relations are not required to be deparsed as
	 * subqueries, but there might be some relations covered by the input
	 * relations that are required to be deparsed as subqueries, so save the
	 * relids of those relations for later use by the deparser.
	 */
	fpinfo->make_outerrel_subquery = false;
	fpinfo->make_innerrel_subquery = false;
	Assert(bms_is_subset(fpinfo_o->lower_subquery_rels, outerrel->relids));
	Assert(bms_is_subset(fpinfo_i->lower_subquery_rels, innerrel->relids));
	fpinfo->lower_subquery_rels = bms_union(fpinfo_o->lower_subquery_rels,
											fpinfo_i->lower_subquery_rels);

	/*
	 * Pull the other remote conditions from the joining relations into join
	 * clauses or other remote clauses (remote_conds) of this relation
	 * wherever possible. This avoids building subqueries at every join step.
	 *
	 * For an inner join, clauses from both the relations are added to the
	 * other remote clauses. For LEFT and RIGHT OUTER join, the clauses from
	 * the outer side are added to remote_conds since those can be evaluated
	 * after the join is evaluated. The clauses from inner side are added to
	 * the joinclauses, since they need to be evaluated while constructing the
	 * join.
	 *
	 * For a FULL OUTER JOIN, the other clauses from either relation can not
	 * be added to the joinclauses or remote_conds, since each relation acts
	 * as an outer relation for the other.
	 *
	 * The joining sides can not have local conditions, thus no need to test
	 * shippability of the clauses being pulled up.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   fpinfo_i->remote_conds);
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   fpinfo_o->remote_conds);
			break;

		case JOIN_LEFT:
			fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
											  fpinfo_i->remote_conds);
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   fpinfo_o->remote_conds);
			break;

		case JOIN_RIGHT:
			fpinfo->joinclauses = list_concat(fpinfo->joinclauses,
											  fpinfo_o->remote_conds);
			fpinfo->remote_conds = list_concat(fpinfo->remote_conds,
											   fpinfo_i->remote_conds);
			break;

		case JOIN_FULL:

			/*
			 * In this case, if any of the input relations has conditions, we
			 * need to deparse that relation as a subquery so that the
			 * conditions can be evaluated before the join.  Remember it in
			 * the fpinfo of this relation so that the deparser can take
			 * appropriate action.  Also, save the relids of base relations
			 * covered by that relation for later use by the deparser.
			 */
			if (fpinfo_o->remote_conds)
			{
				fpinfo->make_outerrel_subquery = true;
				fpinfo->lower_subquery_rels =
					bms_add_members(fpinfo->lower_subquery_rels,
									outerrel->relids);
			}
			if (fpinfo_i->remote_conds)
			{
				fpinfo->make_innerrel_subquery = true;
				fpinfo->lower_subquery_rels =
					bms_add_members(fpinfo->lower_subquery_rels,
									innerrel->relids);
			}
			break;

		default:
			/* Should not happen, we have just checked this above */
			elog(ERROR, "unsupported join type %d", jointype);
	}

	/*
	 * For an inner join, all restrictions can be treated alike. Treating the
	 * pushed down conditions as join conditions allows a top level full outer
	 * join to be deparsed without requiring subqueries.
	 */
	if (jointype == JOIN_INNER)
	{
		Assert(!fpinfo->joinclauses);
		fpinfo->joinclauses = fpinfo->remote_conds;
		fpinfo->remote_conds = NIL;
	}

	/* Mark that this join can be pushed down safely */
	fpinfo->pushdown_safe = true;

	/* Get user mapping */
	if (fpinfo->use_remote_estimate)
	{
		if (fpinfo_o->use_remote_estimate)
			fpinfo->user = fpinfo_o->user;
		else
			fpinfo->user = fpinfo_i->user;
	}
	else
		fpinfo->user = NULL;

	/*
	 * Set # of retrieved rows and cached relation costs to some negative
	 * value, so that we can detect when they are set to some sensible values,
	 * during one (usually the first) of the calls to estimate_path_cost_size.
	 */
	fpinfo->retrieved_rows = -1;
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * Set the string describing this join relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.  Note that the decoration we add
	 * to the base relation names mustn't include any digits, or it'll confuse
	 * postgresExplainForeignScan.
	 */
	fpinfo->relation_name = psprintf("(%s) %s JOIN (%s)",
									 fpinfo_o->relation_name,
									 get_jointype_name(fpinfo->jointype),
									 fpinfo_i->relation_name);

	/*
	 * Set the relation index.  This is defined as the position of this
	 * joinrel in the join_rel_list list plus the length of the rtable list.
	 * Note that since this joinrel is at the end of the join_rel_list list
	 * when we are called, we can get the position by list_length.
	 */
	Assert(fpinfo->relation_index == 0);	/* shouldn't be set yet */
	fpinfo->relation_index =
		list_length(root->parse->rtable) + list_length(root->join_rel_list);

	return true;
}

static void
add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
								Path *epq_path)
{
	List	   *useful_pathkeys_list = NIL; /* List of all pathkeys */
	ListCell   *lc;

	useful_pathkeys_list = get_useful_pathkeys_for_relation(root, rel);

	/*
	 * Before creating sorted paths, arrange for the passed-in EPQ path, if
	 * any, to return columns needed by the parent ForeignScan node so that
	 * they will propagate up through Sort nodes injected below, if necessary.
	 */
	if (epq_path != NULL && useful_pathkeys_list != NIL)
	{
		PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) rel->fdw_private;
		PathTarget *target = copy_pathtarget(epq_path->pathtarget);

		/* Include columns required for evaluating PHVs in the tlist. */
		add_new_columns_to_pathtarget(target,
									  pull_var_clause((Node *) target->exprs,
													  PVC_RECURSE_PLACEHOLDERS));

		/* Include columns required for evaluating the local conditions. */
		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			add_new_columns_to_pathtarget(target,
										  pull_var_clause((Node *) rinfo->clause,
														  PVC_RECURSE_PLACEHOLDERS));
		}

		/*
		 * If we have added any new columns, adjust the tlist of the EPQ path.
		 *
		 * Note: the plan created using this path will only be used to execute
		 * EPQ checks, where accuracy of the plan cost and width estimates
		 * would not be important, so we do not do set_pathtarget_cost_width()
		 * for the new pathtarget here.  See also postgresGetForeignPlan().
		 */
		if (list_length(target->exprs) > list_length(epq_path->pathtarget->exprs))
		{
			/* The EPQ path is a join path, so it is projection-capable. */
			Assert(is_projection_capable_path(epq_path));

			/*
			 * Use create_projection_path() here, so as to avoid modifying it
			 * in place.
			 */
			epq_path = (Path *) create_projection_path(root,
													   rel,
													   epq_path,
													   target);
		}
	}

	/* Create one path for each set of pathkeys we found above. */
	foreach(lc, useful_pathkeys_list)
	{
		double		rows;
		int			width;
		Cost		startup_cost;
		Cost		total_cost;
		List	   *useful_pathkeys = lfirst(lc);
		Path	   *sorted_epq_path;

		estimate_path_cost_size(root, rel, NIL, useful_pathkeys, NULL,
								&rows, &width, &startup_cost, &total_cost);

		/*
		 * The EPQ path must be at least as well sorted as the path itself, in
		 * case it gets used as input to a mergejoin.
		 */
		sorted_epq_path = epq_path;
		if (sorted_epq_path != NULL &&
			!pathkeys_contained_in(useful_pathkeys,
								   sorted_epq_path->pathkeys))
			sorted_epq_path = (Path *)
				create_sort_path(root,
								 rel,
								 sorted_epq_path,
								 useful_pathkeys,
								 -1.0);

		if (IS_SIMPLE_REL(rel))
			add_path(rel, (Path *)
					 create_foreignscan_path(root, rel,
											 NULL,
											 rows,
											 startup_cost,
											 total_cost,
											 useful_pathkeys,
											 rel->lateral_relids,
											 sorted_epq_path,
											 NIL));
		else
			add_path(rel, (Path *)
					 create_foreign_join_path(root, rel,
											  NULL,
											  rows,
											  startup_cost,
											  total_cost,
											  useful_pathkeys,
											  rel->lateral_relids,
											  sorted_epq_path,
											  NIL));
	}
}

/*
 * Parse options from foreign server and apply them to fpinfo.
 *
 * New options might also require tweaking merge_fdw_options().
 */
static void
apply_server_options(PgFdwRelationInfo *fpinfo)
{
	ListCell   *lc;

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
}

/*
 * Parse options from foreign table and apply them to fpinfo.
 *
 * New options might also require tweaking merge_fdw_options().
 */
static void
apply_table_options(PgFdwRelationInfo *fpinfo)
{
	ListCell   *lc;

	foreach(lc, fpinfo->table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_estimate") == 0)
			fpinfo->use_remote_estimate = defGetBoolean(def);
		else if (strcmp(def->defname, "fetch_size") == 0)
			fpinfo->fetch_size = strtol(defGetString(def), NULL, 10);
	}
}

/*
 * Merge FDW options from input relations into a new set of options for a join
 * or an upper rel.
 *
 * For a join relation, FDW-specific information about the inner and outer
 * relations is provided using fpinfo_i and fpinfo_o.  For an upper relation,
 * fpinfo_o provides the information for the input relation; fpinfo_i is
 * expected to NULL.
 */
static void
merge_fdw_options(PgFdwRelationInfo *fpinfo,
				  const PgFdwRelationInfo *fpinfo_o,
				  const PgFdwRelationInfo *fpinfo_i)
{
	/* We must always have fpinfo_o. */
	Assert(fpinfo_o);

	/* fpinfo_i may be NULL, but if present the servers must both match. */
	Assert(!fpinfo_i ||
		   fpinfo_i->server->serverid == fpinfo_o->server->serverid);

	/*
	 * Copy the server specific FDW options.  (For a join, both relations come
	 * from the same server, so the server options should have the same value
	 * for both relations.)
	 */
	fpinfo->fdw_startup_cost = fpinfo_o->fdw_startup_cost;
	fpinfo->fdw_tuple_cost = fpinfo_o->fdw_tuple_cost;
	fpinfo->shippable_extensions = fpinfo_o->shippable_extensions;
	fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate;
	fpinfo->fetch_size = fpinfo_o->fetch_size;

	/* Merge the table level options from either side of the join. */
	if (fpinfo_i)
	{
		/*
		 * We'll prefer to use remote estimates for this join if any table
		 * from either side of the join is using remote estimates.  This is
		 * most likely going to be preferred since they're already willing to
		 * pay the price of a round trip to get the remote EXPLAIN.  In any
		 * case it's not entirely clear how we might otherwise handle this
		 * best.
		 */
		fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate ||
			fpinfo_i->use_remote_estimate;

		/*
		 * Set fetch size to maximum of the joining sides, since we are
		 * expecting the rows returned by the join to be proportional to the
		 * relation sizes.
		 */
		fpinfo->fetch_size = Max(fpinfo_o->fetch_size, fpinfo_i->fetch_size);
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
	 * This code does not work for joins with lateral references, since those
	 * must have parameterized paths, which we don't generate yet.
	 */
	if (!bms_is_empty(joinrel->lateral_relids))
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
	 * If there is a possibility that EvalPlanQual will be executed, we need
	 * to be able to reconstruct the row using scans of the base relations.
	 * GetExistingLocalJoinPath will find a suitable path for this purpose in
	 * the path list of the joinrel, if one exists.  We must be careful to
	 * call it before adding any ForeignPath, since the ForeignPath might
	 * dominate the only suitable local path available.  We also do it before
	 * calling foreign_join_ok(), since that function updates fpinfo and marks
	 * it as pushable if the join is found to be pushable.
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
	 * If we are going to estimate costs locally, estimate the join clause
	 * selectivity here while we have special join info.
	 */
	if (!fpinfo->use_remote_estimate)
		fpinfo->joinclause_sel = clauselist_selectivity(root, fpinfo->joinclauses,
														0, fpinfo->jointype,
														extra->sjinfo);

	/* Estimate costs for bare join relation */
	estimate_path_cost_size(root, joinrel, NIL, NIL, NULL,
							&rows, &width, &startup_cost, &total_cost);
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
	joinpath = create_foreign_join_path(root,
										joinrel,
										NULL,	/* default pathtarget */
										rows,
										startup_cost,
										total_cost,
										NIL,	/* no pathkeys */
										joinrel->lateral_relids,
										epq_path,
										NIL);	/* no fdw_private */

	/* Add generated path into joinrel by add_path(). */
	add_path(joinrel, (Path *) joinpath);

	/* Consider pathkeys for the join relation */
	add_paths_with_pathkeys_for_rel(root, joinrel, epq_path);

	/* XXX Consider parameterized paths for the join relation */
}

/*
 * Assess whether the aggregation, grouping and having operations can be pushed
 * down to the foreign server.  As a side effect, save information we obtain in
 * this function to PgFdwRelationInfo of the input relation.
 */
static bool
foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
					Node *havingQual)
{
	Query	   *query = root->parse;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) grouped_rel->fdw_private;
	PathTarget *grouping_target = grouped_rel->reltarget;
	PgFdwRelationInfo *ofpinfo;
	ListCell   *lc;
	int			i;
	List	   *tlist = NIL;

	/* We currently don't support pushing Grouping Sets. */
	if (query->groupingSets)
		return false;

	/* Get the fpinfo of the underlying scan relation. */
	ofpinfo = (PgFdwRelationInfo *) fpinfo->outerrel->fdw_private;

	/*
	 * If underlying scan relation has any local conditions, those conditions
	 * are required to be applied before performing aggregation.  Hence the
	 * aggregate cannot be pushed down.
	 */
	if (ofpinfo->local_conds)
		return false;

	/*
	 * Examine grouping expressions, as well as other expressions we'd need to
	 * compute, and check whether they are safe to push down to the foreign
	 * server.  All GROUP BY expressions will be part of the grouping target
	 * and thus there is no need to search for them separately.  Add grouping
	 * expressions into target list which will be passed to foreign server.
	 *
	 * A tricky fine point is that we must not put any expression into the
	 * target list that is just a foreign param (that is, something that
	 * deparse.c would conclude has to be sent to the foreign server).  If we
	 * do, the expression will also appear in the fdw_exprs list of the plan
	 * node, and setrefs.c will get confused and decide that the fdw_exprs
	 * entry is actually a reference to the fdw_scan_tlist entry, resulting in
	 * a broken plan.  Somewhat oddly, it's OK if the expression contains such
	 * a node, as long as it's not at top level; then no match is possible.
	 */
	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);
		ListCell   *l;

		/* Check whether this expression is part of GROUP BY clause */
		if (sgref && get_sortgroupref_clause_noerr(sgref, query->groupClause))
		{
			TargetEntry *tle;

			/*
			 * If any GROUP BY expression is not shippable, then we cannot
			 * push down aggregation to the foreign server.
			 */
			if (!is_foreign_expr(root, grouped_rel, expr))
				return false;

			/*
			 * If it would be a foreign param, we can't put it into the tlist,
			 * so we have to fail.
			 */
			if (is_foreign_param(root, grouped_rel, expr))
				return false;

			/*
			 * Pushable, so add to tlist.  We need to create a TLE for this
			 * expression and apply the sortgroupref to it.  We cannot use
			 * add_to_flat_tlist() here because that avoids making duplicate
			 * entries in the tlist.  If there are duplicate entries with
			 * distinct sortgrouprefs, we have to duplicate that situation in
			 * the output tlist.
			 */
			tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
			tle->ressortgroupref = sgref;
			tlist = lappend(tlist, tle);
		}
		else
		{
			/*
			 * Non-grouping expression we need to compute.  Can we ship it
			 * as-is to the foreign server?
			 */
			if (is_foreign_expr(root, grouped_rel, expr) &&
				!is_foreign_param(root, grouped_rel, expr))
			{
				/* Yes, so add to tlist as-is; OK to suppress duplicates */
				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
			else
			{
				/* Not pushable as a whole; extract its Vars and aggregates */
				List	   *aggvars;

				aggvars = pull_var_clause((Node *) expr,
										  PVC_INCLUDE_AGGREGATES);

				/*
				 * If any aggregate expression is not shippable, then we
				 * cannot push down aggregation to the foreign server.  (We
				 * don't have to check is_foreign_param, since that certainly
				 * won't return true for any such expression.)
				 */
				if (!is_foreign_expr(root, grouped_rel, (Expr *) aggvars))
					return false;

				/*
				 * Add aggregates, if any, into the targetlist.  Plain Vars
				 * outside an aggregate can be ignored, because they should be
				 * either same as some GROUP BY column or part of some GROUP
				 * BY expression.  In either case, they are already part of
				 * the targetlist and thus no need to add them again.  In fact
				 * including plain Vars in the tlist when they do not match a
				 * GROUP BY column would cause the foreign server to complain
				 * that the shipped query is invalid.
				 */
				foreach(l, aggvars)
				{
					Expr	   *expr = (Expr *) lfirst(l);

					if (IsA(expr, Aggref))
						tlist = add_to_flat_tlist(tlist, list_make1(expr));
				}
			}
		}

		i++;
	}

	/*
	 * Classify the pushable and non-pushable HAVING clauses and save them in
	 * remote_conds and local_conds of the grouped rel's fpinfo.
	 */
	if (havingQual)
	{
		ListCell   *lc;

		foreach(lc, (List *) havingQual)
		{
			Expr	   *expr = (Expr *) lfirst(lc);
			RestrictInfo *rinfo;

			/*
			 * Currently, the core code doesn't wrap havingQuals in
			 * RestrictInfos, so we must make our own.
			 */
			Assert(!IsA(expr, RestrictInfo));
			rinfo = make_restrictinfo(root,
									  expr,
									  true,
									  false,
									  false,
									  root->qual_security_level,
									  grouped_rel->relids,
									  NULL,
									  NULL);
			if (is_foreign_expr(root, grouped_rel, expr))
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
			else
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
		}
	}

	/*
	 * If there are any local conditions, pull Vars and aggregates from it and
	 * check whether they are safe to pushdown or not.
	 */
	if (fpinfo->local_conds)
	{
		List	   *aggvars = NIL;
		ListCell   *lc;

		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			aggvars = list_concat(aggvars,
								  pull_var_clause((Node *) rinfo->clause,
												  PVC_INCLUDE_AGGREGATES));
		}

		foreach(lc, aggvars)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			/*
			 * If aggregates within local conditions are not safe to push
			 * down, then we cannot push down the query.  Vars are already
			 * part of GROUP BY clause which are checked above, so no need to
			 * access them again here.  Again, we need not check
			 * is_foreign_param for a foreign aggregate.
			 */
			if (IsA(expr, Aggref))
			{
				if (!is_foreign_expr(root, grouped_rel, expr))
					return false;

				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
		}
	}

	/* Store generated targetlist */
	fpinfo->grouped_tlist = tlist;

	/* Safe to pushdown */
	fpinfo->pushdown_safe = true;

	/*
	 * Set # of retrieved rows and cached relation costs to some negative
	 * value, so that we can detect when they are set to some sensible values,
	 * during one (usually the first) of the calls to estimate_path_cost_size.
	 */
	fpinfo->retrieved_rows = -1;
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * Set the string describing this grouped relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.  Note that the decoration we add
	 * to the base relation name mustn't include any digits, or it'll confuse
	 * postgresExplainForeignScan.
	 */
	fpinfo->relation_name = psprintf("Aggregate on (%s)",
									 ofpinfo->relation_name);

	return true;
}

/*
 * postgresGetForeignUpperPaths
 *		Add paths for post-join operations like aggregation, grouping etc. if
 *		corresponding operations are safe to push down.
 */
static void
postgresGetForeignUpperPaths(PlannerInfo *root, UpperRelationKind stage,
							 RelOptInfo *input_rel, RelOptInfo *output_rel,
							 void *extra)
{
	PgFdwRelationInfo *fpinfo;

	/*
	 * If input rel is not safe to pushdown, then simply return as we cannot
	 * perform any post-join operations on the foreign server.
	 */
	if (!input_rel->fdw_private ||
		!((PgFdwRelationInfo *) input_rel->fdw_private)->pushdown_safe)
		return;

	/* Ignore stages we don't support; and skip any duplicate calls. */
	if ((stage != UPPERREL_GROUP_AGG &&
		 stage != UPPERREL_ORDERED &&
		 stage != UPPERREL_FINAL) ||
		output_rel->fdw_private)
		return;

	fpinfo = (PgFdwRelationInfo *) palloc0(sizeof(PgFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	fpinfo->stage = stage;
	output_rel->fdw_private = fpinfo;

	switch (stage)
	{
		case UPPERREL_GROUP_AGG:
			add_foreign_grouping_paths(root, input_rel, output_rel,
									   (GroupPathExtraData *) extra);
			break;
		case UPPERREL_ORDERED:
			add_foreign_ordered_paths(root, input_rel, output_rel);
			break;
		case UPPERREL_FINAL:
			add_foreign_final_paths(root, input_rel, output_rel,
									(FinalPathExtraData *) extra);
			break;
		default:
			elog(ERROR, "unexpected upper relation: %d", (int) stage);
			break;
	}
}

/*
 * add_foreign_grouping_paths
 *		Add foreign path for grouping and/or aggregation.
 *
 * Given input_rel represents the underlying scan.  The paths are added to the
 * given grouped_rel.
 */
static void
add_foreign_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
						   RelOptInfo *grouped_rel,
						   GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	PgFdwRelationInfo *ifpinfo = input_rel->fdw_private;
	PgFdwRelationInfo *fpinfo = grouped_rel->fdw_private;
	ForeignPath *grouppath;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

	/* Nothing to be done, if there is no grouping or aggregation required. */
	if (!parse->groupClause && !parse->groupingSets && !parse->hasAggs &&
		!root->hasHavingQual)
		return;

	Assert(extra->patype == PARTITIONWISE_AGGREGATE_NONE ||
		   extra->patype == PARTITIONWISE_AGGREGATE_FULL);

	/* save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * Assess if it is safe to push down aggregation and grouping.
	 *
	 * Use HAVING qual from extra. In case of child partition, it will have
	 * translated Vars.
	 */
	if (!foreign_grouping_ok(root, grouped_rel, extra->havingQual))
		return;

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path.  (Currently we create just a single
	 * path here, but in future it would be possible that we build more paths
	 * such as pre-sorted paths as in postgresGetForeignPaths and
	 * postgresGetForeignJoinPaths.)  The best we can do for these conditions
	 * is to estimate selectivity on the basis of local statistics.
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 0,
													 JOIN_INNER,
													 NULL);

	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/* Estimate the cost of push down */
	estimate_path_cost_size(root, grouped_rel, NIL, NIL, NULL,
							&rows, &width, &startup_cost, &total_cost);

	/* Now update this information in the fpinfo */
	fpinfo->rows = rows;
	fpinfo->width = width;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	/* Create and add foreign path to the grouping relation. */
	grouppath = create_foreign_upper_path(root,
										  grouped_rel,
										  grouped_rel->reltarget,
										  rows,
										  startup_cost,
										  total_cost,
										  NIL,	/* no pathkeys */
										  NULL,
										  NIL); /* no fdw_private */

	/* Add generated path into grouped_rel by add_path(). */
	add_path(grouped_rel, (Path *) grouppath);
}

/*
 * add_foreign_ordered_paths
 *		Add foreign paths for performing the final sort remotely.
 *
 * Given input_rel contains the source-data Paths.  The paths are added to the
 * given ordered_rel.
 */
static void
add_foreign_ordered_paths(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *ordered_rel)
{
	Query	   *parse = root->parse;
	PgFdwRelationInfo *ifpinfo = input_rel->fdw_private;
	PgFdwRelationInfo *fpinfo = ordered_rel->fdw_private;
	PgFdwPathExtraData *fpextra;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;
	ForeignPath *ordered_path;
	ListCell   *lc;

	/* Shouldn't get here unless the query has ORDER BY */
	Assert(parse->sortClause);

	/* We don't support cases where there are any SRFs in the targetlist */
	if (parse->hasTargetSRFs)
		return;

	/* Save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * If the input_rel is a base or join relation, we would already have
	 * considered pushing down the final sort to the remote server when
	 * creating pre-sorted foreign paths for that relation, because the
	 * query_pathkeys is set to the root->sort_pathkeys in that case (see
	 * standard_qp_callback()).
	 */
	if (input_rel->reloptkind == RELOPT_BASEREL ||
		input_rel->reloptkind == RELOPT_JOINREL)
	{
		Assert(root->query_pathkeys == root->sort_pathkeys);

		/* Safe to push down if the query_pathkeys is safe to push down */
		fpinfo->pushdown_safe = ifpinfo->qp_is_pushdown_safe;

		return;
	}

	/* The input_rel should be a grouping relation */
	Assert(input_rel->reloptkind == RELOPT_UPPER_REL &&
		   ifpinfo->stage == UPPERREL_GROUP_AGG);

	/*
	 * We try to create a path below by extending a simple foreign path for
	 * the underlying grouping relation to perform the final sort remotely,
	 * which is stored into the fdw_private list of the resulting path.
	 */

	/* Assess if it is safe to push down the final sort */
	foreach(lc, root->sort_pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc);
		EquivalenceClass *pathkey_ec = pathkey->pk_eclass;

		/*
		 * is_foreign_expr would detect volatile expressions as well, but
		 * checking ec_has_volatile here saves some cycles.
		 */
		if (pathkey_ec->ec_has_volatile)
			return;

		/*
		 * Can't push down the sort if pathkey's opfamily is not shippable.
		 */
		if (!is_shippable(pathkey->pk_opfamily, OperatorFamilyRelationId,
						  fpinfo))
			return;

		/*
		 * The EC must contain a shippable EM that is computed in input_rel's
		 * reltarget, else we can't push down the sort.
		 */
		if (find_em_for_rel_target(root,
								   pathkey_ec,
								   input_rel) == NULL)
			return;
	}

	/* Safe to push down */
	fpinfo->pushdown_safe = true;

	/* Construct PgFdwPathExtraData */
	fpextra = (PgFdwPathExtraData *) palloc0(sizeof(PgFdwPathExtraData));
	fpextra->target = root->upper_targets[UPPERREL_ORDERED];
	fpextra->has_final_sort = true;

	/* Estimate the costs of performing the final sort remotely */
	estimate_path_cost_size(root, input_rel, NIL, root->sort_pathkeys, fpextra,
							&rows, &width, &startup_cost, &total_cost);

	/*
	 * Build the fdw_private list that will be used by postgresGetForeignPlan.
	 * Items in the list must match order in enum FdwPathPrivateIndex.
	 */
	fdw_private = list_make2(makeInteger(true), makeInteger(false));

	/* Create foreign ordering path */
	ordered_path = create_foreign_upper_path(root,
											 input_rel,
											 root->upper_targets[UPPERREL_ORDERED],
											 rows,
											 startup_cost,
											 total_cost,
											 root->sort_pathkeys,
											 NULL,	/* no extra plan */
											 fdw_private);

	/* and add it to the ordered_rel */
	add_path(ordered_rel, (Path *) ordered_path);
}

/*
 * add_foreign_final_paths
 *		Add foreign paths for performing the final processing remotely.
 *
 * Given input_rel contains the source-data Paths.  The paths are added to the
 * given final_rel.
 */
static void
add_foreign_final_paths(PlannerInfo *root, RelOptInfo *input_rel,
						RelOptInfo *final_rel,
						FinalPathExtraData *extra)
{
	Query	   *parse = root->parse;
	PgFdwRelationInfo *ifpinfo = (PgFdwRelationInfo *) input_rel->fdw_private;
	PgFdwRelationInfo *fpinfo = (PgFdwRelationInfo *) final_rel->fdw_private;
	bool		has_final_sort = false;
	List	   *pathkeys = NIL;
	PgFdwPathExtraData *fpextra;
	bool		save_use_remote_estimate = false;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;
	ForeignPath *final_path;

	/*
	 * Currently, we only support this for SELECT commands
	 */
	if (parse->commandType != CMD_SELECT)
		return;

	/*
	 * No work if there is no FOR UPDATE/SHARE clause and if there is no need
	 * to add a LIMIT node
	 */
	if (!parse->rowMarks && !extra->limit_needed)
		return;

	/* We don't support cases where there are any SRFs in the targetlist */
	if (parse->hasTargetSRFs)
		return;

	/* Save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * If there is no need to add a LIMIT node, there might be a ForeignPath
	 * in the input_rel's pathlist that implements all behavior of the query.
	 * Note: we would already have accounted for the query's FOR UPDATE/SHARE
	 * (if any) before we get here.
	 */
	if (!extra->limit_needed)
	{
		ListCell   *lc;

		Assert(parse->rowMarks);

		/*
		 * Grouping and aggregation are not supported with FOR UPDATE/SHARE,
		 * so the input_rel should be a base, join, or ordered relation; and
		 * if it's an ordered relation, its input relation should be a base or
		 * join relation.
		 */
		Assert(input_rel->reloptkind == RELOPT_BASEREL ||
			   input_rel->reloptkind == RELOPT_JOINREL ||
			   (input_rel->reloptkind == RELOPT_UPPER_REL &&
				ifpinfo->stage == UPPERREL_ORDERED &&
				(ifpinfo->outerrel->reloptkind == RELOPT_BASEREL ||
				 ifpinfo->outerrel->reloptkind == RELOPT_JOINREL)));

		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			/*
			 * apply_scanjoin_target_to_paths() uses create_projection_path()
			 * to adjust each of its input paths if needed, whereas
			 * create_ordered_paths() uses apply_projection_to_path() to do
			 * that.  So the former might have put a ProjectionPath on top of
			 * the ForeignPath; look through ProjectionPath and see if the
			 * path underneath it is ForeignPath.
			 */
			if (IsA(path, ForeignPath) ||
				(IsA(path, ProjectionPath) &&
				 IsA(((ProjectionPath *) path)->subpath, ForeignPath)))
			{
				/*
				 * Create foreign final path; this gets rid of a
				 * no-longer-needed outer plan (if any), which makes the
				 * EXPLAIN output look cleaner
				 */
				final_path = create_foreign_upper_path(root,
													   path->parent,
													   path->pathtarget,
													   path->rows,
													   path->startup_cost,
													   path->total_cost,
													   path->pathkeys,
													   NULL,	/* no extra plan */
													   NULL);	/* no fdw_private */

				/* and add it to the final_rel */
				add_path(final_rel, (Path *) final_path);

				/* Safe to push down */
				fpinfo->pushdown_safe = true;

				return;
			}
		}

		/*
		 * If we get here it means no ForeignPaths; since we would already
		 * have considered pushing down all operations for the query to the
		 * remote server, give up on it.
		 */
		return;
	}

	Assert(extra->limit_needed);

	/*
	 * If the input_rel is an ordered relation, replace the input_rel with its
	 * input relation
	 */
	if (input_rel->reloptkind == RELOPT_UPPER_REL &&
		ifpinfo->stage == UPPERREL_ORDERED)
	{
		input_rel = ifpinfo->outerrel;
		ifpinfo = (PgFdwRelationInfo *) input_rel->fdw_private;
		has_final_sort = true;
		pathkeys = root->sort_pathkeys;
	}

	/* The input_rel should be a base, join, or grouping relation */
	Assert(input_rel->reloptkind == RELOPT_BASEREL ||
		   input_rel->reloptkind == RELOPT_JOINREL ||
		   (input_rel->reloptkind == RELOPT_UPPER_REL &&
			ifpinfo->stage == UPPERREL_GROUP_AGG));

	/*
	 * We try to create a path below by extending a simple foreign path for
	 * the underlying base, join, or grouping relation to perform the final
	 * sort (if has_final_sort) and the LIMIT restriction remotely, which is
	 * stored into the fdw_private list of the resulting path.  (We
	 * re-estimate the costs of sorting the underlying relation, if
	 * has_final_sort.)
	 */

	/*
	 * Assess if it is safe to push down the LIMIT and OFFSET to the remote
	 * server
	 */

	/*
	 * If the underlying relation has any local conditions, the LIMIT/OFFSET
	 * cannot be pushed down.
	 */
	if (ifpinfo->local_conds)
		return;

	/*
	 * Also, the LIMIT/OFFSET cannot be pushed down, if their expressions are
	 * not safe to remote.
	 */
	if (!is_foreign_expr(root, input_rel, (Expr *) parse->limitOffset) ||
		!is_foreign_expr(root, input_rel, (Expr *) parse->limitCount))
		return;

	/* Safe to push down */
	fpinfo->pushdown_safe = true;

	/* Construct PgFdwPathExtraData */
	fpextra = (PgFdwPathExtraData *) palloc0(sizeof(PgFdwPathExtraData));
	fpextra->target = root->upper_targets[UPPERREL_FINAL];
	fpextra->has_final_sort = has_final_sort;
	fpextra->has_limit = extra->limit_needed;
	fpextra->limit_tuples = extra->limit_tuples;
	fpextra->count_est = extra->count_est;
	fpextra->offset_est = extra->offset_est;

	/*
	 * Estimate the costs of performing the final sort and the LIMIT
	 * restriction remotely.  If has_final_sort is false, we wouldn't need to
	 * execute EXPLAIN anymore if use_remote_estimate, since the costs can be
	 * roughly estimated using the costs we already have for the underlying
	 * relation, in the same way as when use_remote_estimate is false.  Since
	 * it's pretty expensive to execute EXPLAIN, force use_remote_estimate to
	 * false in that case.
	 */
	if (!fpextra->has_final_sort)
	{
		save_use_remote_estimate = ifpinfo->use_remote_estimate;
		ifpinfo->use_remote_estimate = false;
	}
	estimate_path_cost_size(root, input_rel, NIL, pathkeys, fpextra,
							&rows, &width, &startup_cost, &total_cost);
	if (!fpextra->has_final_sort)
		ifpinfo->use_remote_estimate = save_use_remote_estimate;

	/*
	 * Build the fdw_private list that will be used by postgresGetForeignPlan.
	 * Items in the list must match order in enum FdwPathPrivateIndex.
	 */
	fdw_private = list_make2(makeInteger(has_final_sort),
							 makeInteger(extra->limit_needed));

	/*
	 * Create foreign final path; this gets rid of a no-longer-needed outer
	 * plan (if any), which makes the EXPLAIN output look cleaner
	 */
	final_path = create_foreign_upper_path(root,
										   input_rel,
										   root->upper_targets[UPPERREL_FINAL],
										   rows,
										   startup_cost,
										   total_cost,
										   pathkeys,
										   NULL,	/* no extra plan */
										   fdw_private);

	/* and add it to the final_rel */
	add_path(final_rel, (Path *) final_path);
}

/*
 * Create a tuple from the specified row of the PGresult.
 *
 * rel is the local representation of the foreign table, attinmeta is
 * conversion data for the rel's tupdesc, and retrieved_attrs is an
 * integer list of the table column numbers present in the PGresult.
 * fsstate is the ForeignScan plan node's execution state.
 * temp_context is a working context that can be reset after each tuple.
 *
 * Note: either rel or fsstate, but not both, can be NULL.  rel is NULL
 * if we're processing a remote join, while fsstate is NULL in a non-query
 * context such as ANALYZE, or if we're processing a non-scan query node.
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

	/*
	 * Get the tuple descriptor for the row.  Use the rel's tupdesc if rel is
	 * provided, otherwise look to the scan node's ScanTupleSlot.
	 */
	if (rel)
		tupdesc = RelationGetDescr(rel);
	else
	{
		Assert(fsstate);
		tupdesc = fsstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	}

	values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));
	/* Initialize to nulls for any columns not present in result */
	memset(nulls, true, tupdesc->natts * sizeof(bool));

	/*
	 * Set up and install callback to report where conversion error occurs.
	 */
	errpos.cur_attno = 0;
	errpos.rel = rel;
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

		/*
		 * convert value to internal representation
		 *
		 * Note: we ignore system columns other than ctid and oid in result
		 */
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
			/* ctid */
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

	/*
	 * Stomp on the xmin, xmax, and cmin fields from the tuple created by
	 * heap_form_tuple.  heap_form_tuple actually creates the tuple with
	 * DatumTupleFields, not HeapTupleFields, but the executor expects
	 * HeapTupleFields and will happily extract system columns on that
	 * assumption.  If we don't do this then, for example, the tuple length
	 * ends up in the xmin field, which isn't what we want.
	 */
	HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);
	HeapTupleHeaderSetXmin(tuple->t_data, InvalidTransactionId);
	HeapTupleHeaderSetCmin(tuple->t_data, InvalidTransactionId);

	/* Clean up */
	MemoryContextReset(temp_context);

	return tuple;
}

/*
 * Callback function which is called when error occurs during column value
 * conversion.  Print names of column and relation.
 *
 * Note that this function mustn't do any catalog lookups, since we are in
 * an already-failed transaction.  Fortunately, we can get the needed info
 * from the relation or the query's rangetable instead.
 */
static void
conversion_error_callback(void *arg)
{
	ConversionLocation *errpos = (ConversionLocation *) arg;
	Relation	rel = errpos->rel;
	ForeignScanState *fsstate = errpos->fsstate;
	const char *attname = NULL;
	const char *relname = NULL;
	bool		is_wholerow = false;

	/*
	 * If we're in a scan node, always use aliases from the rangetable, for
	 * consistency between the simple-relation and remote-join cases.  Look at
	 * the relation's tupdesc only if we're not in a scan node.
	 */
	if (fsstate)
	{
		/* ForeignScan case */
		ForeignScan *fsplan = castNode(ForeignScan, fsstate->ss.ps.plan);
		int			varno = 0;
		AttrNumber	colno = 0;

		if (fsplan->scan.scanrelid > 0)
		{
			/* error occurred in a scan against a foreign table */
			varno = fsplan->scan.scanrelid;
			colno = errpos->cur_attno;
		}
		else
		{
			/* error occurred in a scan against a foreign join */
			TargetEntry *tle;

			tle = list_nth_node(TargetEntry, fsplan->fdw_scan_tlist,
								errpos->cur_attno - 1);

			/*
			 * Target list can have Vars and expressions.  For Vars, we can
			 * get some information, however for expressions we can't.  Thus
			 * for expressions, just show generic context message.
			 */
			if (IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;

				varno = var->varno;
				colno = var->varattno;
			}
		}

		if (varno > 0)
		{
			EState	   *estate = fsstate->ss.ps.state;
			RangeTblEntry *rte = exec_rt_fetch(varno, estate);

			relname = rte->eref->aliasname;

			if (colno == 0)
				is_wholerow = true;
			else if (colno > 0 && colno <= list_length(rte->eref->colnames))
				attname = strVal(list_nth(rte->eref->colnames, colno - 1));
			else if (colno == SelfItemPointerAttributeNumber)
				attname = "ctid";
		}
	}
	else if (rel)
	{
		/* Non-ForeignScan case (we should always have a rel here) */
		TupleDesc	tupdesc = RelationGetDescr(rel);

		relname = RelationGetRelationName(rel);
		if (errpos->cur_attno > 0 && errpos->cur_attno <= tupdesc->natts)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc,
												   errpos->cur_attno - 1);

			attname = NameStr(attr->attname);
		}
		else if (errpos->cur_attno == SelfItemPointerAttributeNumber)
			attname = "ctid";
	}

	if (relname && is_wholerow)
		errcontext("whole-row reference to foreign table \"%s\"", relname);
	else if (relname && attname)
		errcontext("column \"%s\" of foreign table \"%s\"", attname, relname);
	else
		errcontext("processing expression at position %d in select list",
				   errpos->cur_attno);
}

/*
 * Given an EquivalenceClass and a foreign relation, find an EC member
 * that can be used to sort the relation remotely according to a pathkey
 * using this EC.
 *
 * If there is more than one suitable candidate, return an arbitrary
 * one of them.  If there is none, return NULL.
 *
 * This checks that the EC member expression uses only Vars from the given
 * rel and is shippable.  Caller must separately verify that the pathkey's
 * ordering operator is shippable.
 */
EquivalenceMember *
find_em_for_rel(PlannerInfo *root, EquivalenceClass *ec, RelOptInfo *rel)
{
	ListCell   *lc;

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);

		/*
		 * Note we require !bms_is_empty, else we'd accept constant
		 * expressions which are not suitable for the purpose.
		 */
		if (bms_is_subset(em->em_relids, rel->relids) &&
			!bms_is_empty(em->em_relids) &&
			is_foreign_expr(root, rel, em->em_expr))
			return em;
	}

	return NULL;
}

/*
 * Find an EquivalenceClass member that is to be computed as a sort column
 * in the given rel's reltarget, and is shippable.
 *
 * If there is more than one suitable candidate, return an arbitrary
 * one of them.  If there is none, return NULL.
 *
 * This checks that the EC member expression uses only Vars from the given
 * rel and is shippable.  Caller must separately verify that the pathkey's
 * ordering operator is shippable.
 */
EquivalenceMember *
find_em_for_rel_target(PlannerInfo *root, EquivalenceClass *ec,
					   RelOptInfo *rel)
{
	PathTarget *target = rel->reltarget;
	ListCell   *lc1;
	int			i;

	i = 0;
	foreach(lc1, target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc1);
		Index		sgref = get_pathtarget_sortgroupref(target, i);
		ListCell   *lc2;

		/* Ignore non-sort expressions */
		if (sgref == 0 ||
			get_sortgroupref_clause_noerr(sgref,
										  root->parse->sortClause) == NULL)
		{
			i++;
			continue;
		}

		/* We ignore binary-compatible relabeling on both ends */
		while (expr && IsA(expr, RelabelType))
			expr = ((RelabelType *) expr)->arg;

		/* Locate an EquivalenceClass member matching this expr, if any */
		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);
			Expr	   *em_expr;

			/* Don't match constants */
			if (em->em_is_const)
				continue;

			/* Ignore child members */
			if (em->em_is_child)
				continue;

			/* Match if same expression (after stripping relabel) */
			em_expr = em->em_expr;
			while (em_expr && IsA(em_expr, RelabelType))
				em_expr = ((RelabelType *) em_expr)->arg;

			if (!equal(em_expr, expr))
				continue;

			/* Check that expression (including relabels!) is shippable */
			if (is_foreign_expr(root, rel, em->em_expr))
				return em;
		}

		i++;
	}

	return NULL;
}
