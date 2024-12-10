/*-------------------------------------------------------------------------
 *
 * file_fdw.c
 *		  foreign-data wrapper for server-side flat files (or programs).
 *
 * Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/file_fdw/file_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_foreign_table.h"
#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/acl.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct FileFdwOption
{
	const char *optname;
	Oid			optcontext;		/* Oid of catalog in which option may appear */
};

/*
 * Valid options for file_fdw.
 * These options are based on the options for the COPY FROM command.
 * But note that force_not_null and force_null are handled as boolean options
 * attached to a column, not as table options.
 *
 * Note: If you are adding new option for user mapping, you need to modify
 * fileGetOptions(), which currently doesn't bother to look at user mappings.
 */
static const struct FileFdwOption valid_options[] = {
	/* Data source options */
	{"filename", ForeignTableRelationId},
	{"program", ForeignTableRelationId},

	/* Format options */
	/* oids option is not supported */
	{"format", ForeignTableRelationId},
	{"header", ForeignTableRelationId},
	{"delimiter", ForeignTableRelationId},
	{"quote", ForeignTableRelationId},
	{"escape", ForeignTableRelationId},
	{"null", ForeignTableRelationId},
	{"default", ForeignTableRelationId},
	{"encoding", ForeignTableRelationId},
	{"on_error", ForeignTableRelationId},
	{"log_verbosity", ForeignTableRelationId},
	{"reject_limit", ForeignTableRelationId},
	{"force_not_null", AttributeRelationId},
	{"force_null", AttributeRelationId},

	/*
	 * force_quote is not supported by file_fdw because it's for COPY TO.
	 */

	/* Sentinel */
	{NULL, InvalidOid}
};

/*
 * FDW-specific information for RelOptInfo.fdw_private.
 */
typedef struct FileFdwPlanState
{
	char	   *filename;		/* file or program to read from */
	bool		is_program;		/* true if filename represents an OS command */
	List	   *options;		/* merged COPY options, excluding filename and
								 * is_program */
	BlockNumber pages;			/* estimate of file's physical size */
	double		ntuples;		/* estimate of number of data rows */
} FileFdwPlanState;

/*
 * FDW-specific information for ForeignScanState.fdw_state.
 */
typedef struct FileFdwExecutionState
{
	char	   *filename;		/* file or program to read from */
	bool		is_program;		/* true if filename represents an OS command */
	List	   *options;		/* merged COPY options, excluding filename and
								 * is_program */
	CopyFromState cstate;		/* COPY execution state */
} FileFdwExecutionState;

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(file_fdw_handler);
PG_FUNCTION_INFO_V1(file_fdw_validator);

/*
 * FDW callback routines
 */
static void fileGetForeignRelSize(PlannerInfo *root,
								  RelOptInfo *baserel,
								  Oid foreigntableid);
static void fileGetForeignPaths(PlannerInfo *root,
								RelOptInfo *baserel,
								Oid foreigntableid);
static ForeignScan *fileGetForeignPlan(PlannerInfo *root,
									   RelOptInfo *baserel,
									   Oid foreigntableid,
									   ForeignPath *best_path,
									   List *tlist,
									   List *scan_clauses,
									   Plan *outer_plan);
static void fileExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void fileBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *fileIterateForeignScan(ForeignScanState *node);
static void fileReScanForeignScan(ForeignScanState *node);
static void fileEndForeignScan(ForeignScanState *node);
static bool fileAnalyzeForeignTable(Relation relation,
									AcquireSampleRowsFunc *func,
									BlockNumber *totalpages);
static bool fileIsForeignScanParallelSafe(PlannerInfo *root, RelOptInfo *rel,
										  RangeTblEntry *rte);

/*
 * Helper functions
 */
static bool is_valid_option(const char *option, Oid context);
static void fileGetOptions(Oid foreigntableid,
						   char **filename,
						   bool *is_program,
						   List **other_options);
static List *get_file_fdw_attribute_options(Oid relid);
static bool check_selective_binary_conversion(RelOptInfo *baserel,
											  Oid foreigntableid,
											  List **columns);
static void estimate_size(PlannerInfo *root, RelOptInfo *baserel,
						  FileFdwPlanState *fdw_private);
static void estimate_costs(PlannerInfo *root, RelOptInfo *baserel,
						   FileFdwPlanState *fdw_private,
						   Cost *startup_cost, Cost *total_cost);
static int	file_acquire_sample_rows(Relation onerel, int elevel,
									 HeapTuple *rows, int targrows,
									 double *totalrows, double *totaldeadrows);


/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
file_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	fdwroutine->GetForeignRelSize = fileGetForeignRelSize;
	fdwroutine->GetForeignPaths = fileGetForeignPaths;
	fdwroutine->GetForeignPlan = fileGetForeignPlan;
	fdwroutine->ExplainForeignScan = fileExplainForeignScan;
	fdwroutine->BeginForeignScan = fileBeginForeignScan;
	fdwroutine->IterateForeignScan = fileIterateForeignScan;
	fdwroutine->ReScanForeignScan = fileReScanForeignScan;
	fdwroutine->EndForeignScan = fileEndForeignScan;
	fdwroutine->AnalyzeForeignTable = fileAnalyzeForeignTable;
	fdwroutine->IsForeignScanParallelSafe = fileIsForeignScanParallelSafe;

	PG_RETURN_POINTER(fdwroutine);
}

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses file_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
Datum
file_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	char	   *filename = NULL;
	DefElem    *force_not_null = NULL;
	DefElem    *force_null = NULL;
	List	   *other_options = NIL;
	ListCell   *cell;

	/*
	 * Check that only options supported by file_fdw, and allowed for the
	 * current object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			const struct FileFdwOption *opt;
			const char *closest_match;
			ClosestMatchState match_state;
			bool		has_valid_options = false;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with a valid option that looks similar, if there is one.
			 */
			initClosestMatch(&match_state, def->defname, 4);
			for (opt = valid_options; opt->optname; opt++)
			{
				if (catalog == opt->optcontext)
				{
					has_valid_options = true;
					updateClosestMatch(&match_state, opt->optname);
				}
			}

			closest_match = getClosestMatch(&match_state);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 has_valid_options ? closest_match ?
					 errhint("Perhaps you meant the option \"%s\".",
							 closest_match) : 0 :
					 errhint("There are no valid options in this context.")));
		}

		/*
		 * Separate out filename, program, and column-specific options, since
		 * ProcessCopyOptions won't accept them.
		 */
		if (strcmp(def->defname, "filename") == 0 ||
			strcmp(def->defname, "program") == 0)
		{
			if (filename)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			/*
			 * Check permissions for changing which file or program is used by
			 * the file_fdw.
			 *
			 * Only members of the role 'pg_read_server_files' are allowed to
			 * set the 'filename' option of a file_fdw foreign table, while
			 * only members of the role 'pg_execute_server_program' are
			 * allowed to set the 'program' option.  This is because we don't
			 * want regular users to be able to control which file gets read
			 * or which program gets executed.
			 *
			 * Putting this sort of permissions check in a validator is a bit
			 * of a crock, but there doesn't seem to be any other place that
			 * can enforce the check more cleanly.
			 *
			 * Note that the valid_options[] array disallows setting filename
			 * and program at any options level other than foreign table ---
			 * otherwise there'd still be a security hole.
			 */
			if (strcmp(def->defname, "filename") == 0 &&
				!has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set the \"%s\" option of a file_fdw foreign table",
								"filename"),
						 errdetail("Only roles with privileges of the \"%s\" role may set this option.",
								   "pg_read_server_files")));

			if (strcmp(def->defname, "program") == 0 &&
				!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set the \"%s\" option of a file_fdw foreign table",
								"program"),
						 errdetail("Only roles with privileges of the \"%s\" role may set this option.",
								   "pg_execute_server_program")));

			filename = defGetString(def);
		}

		/*
		 * force_not_null is a boolean option; after validation we can discard
		 * it - it will be retrieved later in get_file_fdw_attribute_options()
		 */
		else if (strcmp(def->defname, "force_not_null") == 0)
		{
			if (force_not_null)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 errhint("Option \"force_not_null\" supplied more than once for a column.")));
			force_not_null = def;
			/* Don't care what the value is, as long as it's a legal boolean */
			(void) defGetBoolean(def);
		}
		/* See comments for force_not_null above */
		else if (strcmp(def->defname, "force_null") == 0)
		{
			if (force_null)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options"),
						 errhint("Option \"force_null\" supplied more than once for a column.")));
			force_null = def;
			(void) defGetBoolean(def);
		}
		else
			other_options = lappend(other_options, def);
	}

	/*
	 * Now apply the core COPY code's validation logic for more checks.
	 */
	ProcessCopyOptions(NULL, NULL, true, other_options);

	/*
	 * Either filename or program option is required for file_fdw foreign
	 * tables.
	 */
	if (catalog == ForeignTableRelationId && filename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
				 errmsg("either filename or program is required for file_fdw foreign tables")));

	PG_RETURN_VOID();
}

/*
 * Check if the provided option is one of the valid options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
is_valid_option(const char *option, Oid context)
{
	const struct FileFdwOption *opt;

	for (opt = valid_options; opt->optname; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->optname, option) == 0)
			return true;
	}
	return false;
}

/*
 * Fetch the options for a file_fdw foreign table.
 *
 * We have to separate out filename/program from the other options because
 * those must not appear in the options list passed to the core COPY code.
 */
static void
fileGetOptions(Oid foreigntableid,
			   char **filename, bool *is_program, List **other_options)
{
	ForeignTable *table;
	ForeignServer *server;
	ForeignDataWrapper *wrapper;
	List	   *options;
	ListCell   *lc;

	/*
	 * Extract options from FDW objects.  We ignore user mappings because
	 * file_fdw doesn't have any options that can be specified there.
	 *
	 * (XXX Actually, given the current contents of valid_options[], there's
	 * no point in examining anything except the foreign table's own options.
	 * Simplify?)
	 */
	table = GetForeignTable(foreigntableid);
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);

	options = NIL;
	options = list_concat(options, wrapper->options);
	options = list_concat(options, server->options);
	options = list_concat(options, table->options);
	options = list_concat(options, get_file_fdw_attribute_options(foreigntableid));

	/*
	 * Separate out the filename or program option (we assume there is only
	 * one).
	 */
	*filename = NULL;
	*is_program = false;
	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "filename") == 0)
		{
			*filename = defGetString(def);
			options = foreach_delete_current(options, lc);
			break;
		}
		else if (strcmp(def->defname, "program") == 0)
		{
			*filename = defGetString(def);
			*is_program = true;
			options = foreach_delete_current(options, lc);
			break;
		}
	}

	/*
	 * The validator should have checked that filename or program was included
	 * in the options, but check again, just in case.
	 */
	if (*filename == NULL)
		elog(ERROR, "either filename or program is required for file_fdw foreign tables");

	*other_options = options;
}

/*
 * Retrieve per-column generic options from pg_attribute and construct a list
 * of DefElems representing them.
 *
 * At the moment we only have "force_not_null", and "force_null",
 * which should each be combined into a single DefElem listing all such
 * columns, since that's what COPY expects.
 */
static List *
get_file_fdw_attribute_options(Oid relid)
{
	Relation	rel;
	TupleDesc	tupleDesc;
	AttrNumber	natts;
	AttrNumber	attnum;
	List	   *fnncolumns = NIL;
	List	   *fncolumns = NIL;

	List	   *options = NIL;

	rel = table_open(relid, AccessShareLock);
	tupleDesc = RelationGetDescr(rel);
	natts = tupleDesc->natts;

	/* Retrieve FDW options for all user-defined attributes. */
	for (attnum = 1; attnum <= natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupleDesc, attnum - 1);
		List	   *column_options;
		ListCell   *lc;

		/* Skip dropped attributes. */
		if (attr->attisdropped)
			continue;

		column_options = GetForeignColumnOptions(relid, attnum);
		foreach(lc, column_options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "force_not_null") == 0)
			{
				if (defGetBoolean(def))
				{
					char	   *attname = pstrdup(NameStr(attr->attname));

					fnncolumns = lappend(fnncolumns, makeString(attname));
				}
			}
			else if (strcmp(def->defname, "force_null") == 0)
			{
				if (defGetBoolean(def))
				{
					char	   *attname = pstrdup(NameStr(attr->attname));

					fncolumns = lappend(fncolumns, makeString(attname));
				}
			}
			/* maybe in future handle other column options here */
		}
	}

	table_close(rel, AccessShareLock);

	/*
	 * Return DefElem only when some column(s) have force_not_null /
	 * force_null options set
	 */
	if (fnncolumns != NIL)
		options = lappend(options, makeDefElem("force_not_null", (Node *) fnncolumns, -1));

	if (fncolumns != NIL)
		options = lappend(options, makeDefElem("force_null", (Node *) fncolumns, -1));

	return options;
}

/*
 * fileGetForeignRelSize
 *		Obtain relation size estimates for a foreign table
 */
static void
fileGetForeignRelSize(PlannerInfo *root,
					  RelOptInfo *baserel,
					  Oid foreigntableid)
{
	FileFdwPlanState *fdw_private;

	/*
	 * Fetch options.  We only need filename (or program) at this point, but
	 * we might as well get everything and not need to re-fetch it later in
	 * planning.
	 */
	fdw_private = (FileFdwPlanState *) palloc(sizeof(FileFdwPlanState));
	fileGetOptions(foreigntableid,
				   &fdw_private->filename,
				   &fdw_private->is_program,
				   &fdw_private->options);
	baserel->fdw_private = fdw_private;

	/* Estimate relation size */
	estimate_size(root, baserel, fdw_private);
}

/*
 * fileGetForeignPaths
 *		Create possible access paths for a scan on the foreign table
 *
 *		Currently we don't support any push-down feature, so there is only one
 *		possible access path, which simply returns all records in the order in
 *		the data file.
 */
static void
fileGetForeignPaths(PlannerInfo *root,
					RelOptInfo *baserel,
					Oid foreigntableid)
{
	FileFdwPlanState *fdw_private = (FileFdwPlanState *) baserel->fdw_private;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *columns;
	List	   *coptions = NIL;

	/* Decide whether to selectively perform binary conversion */
	if (check_selective_binary_conversion(baserel,
										  foreigntableid,
										  &columns))
		coptions = list_make1(makeDefElem("convert_selectively",
										  (Node *) columns, -1));

	/* Estimate costs */
	estimate_costs(root, baserel, fdw_private,
				   &startup_cost, &total_cost);

	/*
	 * Create a ForeignPath node and add it as only possible path.  We use the
	 * fdw_private list of the path to carry the convert_selectively option;
	 * it will be propagated into the fdw_private list of the Plan node.
	 *
	 * We don't support pushing join clauses into the quals of this path, but
	 * it could still have required parameterization due to LATERAL refs in
	 * its tlist.
	 */
	add_path(baserel, (Path *)
			 create_foreignscan_path(root, baserel,
									 NULL,	/* default pathtarget */
									 baserel->rows,
									 0,
									 startup_cost,
									 total_cost,
									 NIL,	/* no pathkeys */
									 baserel->lateral_relids,
									 NULL,	/* no extra plan */
									 NIL,	/* no fdw_restrictinfo list */
									 coptions));

	/*
	 * If data file was sorted, and we knew it somehow, we could insert
	 * appropriate pathkeys into the ForeignPath node to tell the planner
	 * that.
	 */
}

/*
 * fileGetForeignPlan
 *		Create a ForeignScan plan node for scanning the foreign table
 */
static ForeignScan *
fileGetForeignPlan(PlannerInfo *root,
				   RelOptInfo *baserel,
				   Oid foreigntableid,
				   ForeignPath *best_path,
				   List *tlist,
				   List *scan_clauses,
				   Plan *outer_plan)
{
	Index		scan_relid = baserel->relid;

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check.  So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 */
	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Create the ForeignScan node */
	return make_foreignscan(tlist,
							scan_clauses,
							scan_relid,
							NIL,	/* no expressions to evaluate */
							best_path->fdw_private,
							NIL,	/* no custom tlist */
							NIL,	/* no remote quals */
							outer_plan);
}

/*
 * fileExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
fileExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	char	   *filename;
	bool		is_program;
	List	   *options;

	/* Fetch options --- we only need filename and is_program at this point */
	fileGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				   &filename, &is_program, &options);

	if (is_program)
		ExplainPropertyText("Foreign Program", filename, es);
	else
		ExplainPropertyText("Foreign File", filename, es);

	/* Suppress file size if we're not showing cost details */
	if (es->costs)
	{
		struct stat stat_buf;

		if (!is_program &&
			stat(filename, &stat_buf) == 0)
			ExplainPropertyInteger("Foreign File Size", "b",
								   (int64) stat_buf.st_size, es);
	}
}

/*
 * fileBeginForeignScan
 *		Initiate access to the file by creating CopyState
 */
static void
fileBeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *plan = (ForeignScan *) node->ss.ps.plan;
	char	   *filename;
	bool		is_program;
	List	   *options;
	CopyFromState cstate;
	FileFdwExecutionState *festate;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  node->fdw_state stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/* Fetch options of foreign table */
	fileGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				   &filename, &is_program, &options);

	/* Add any options from the plan (currently only convert_selectively) */
	options = list_concat(options, plan->fdw_private);

	/*
	 * Create CopyState from FDW options.  We always acquire all columns, so
	 * as to match the expected ScanTupleSlot signature.
	 */
	cstate = BeginCopyFrom(NULL,
						   node->ss.ss_currentRelation,
						   NULL,
						   filename,
						   is_program,
						   NULL,
						   NIL,
						   options);

	/*
	 * Save state in node->fdw_state.  We must save enough information to call
	 * BeginCopyFrom() again.
	 */
	festate = (FileFdwExecutionState *) palloc(sizeof(FileFdwExecutionState));
	festate->filename = filename;
	festate->is_program = is_program;
	festate->options = options;
	festate->cstate = cstate;

	node->fdw_state = festate;
}

/*
 * fileIterateForeignScan
 *		Read next record from the data file and store it into the
 *		ScanTupleSlot as a virtual tuple
 */
static TupleTableSlot *
fileIterateForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext;
	MemoryContext oldcontext = CurrentMemoryContext;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	CopyFromState cstate = festate->cstate;
	ErrorContextCallback errcallback;

	/* Set up callback to identify error line number. */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/*
	 * We pass ExprContext because there might be a use of the DEFAULT option
	 * in COPY FROM, so we may need to evaluate default expressions.
	 */
	econtext = GetPerTupleExprContext(estate);

retry:

	/*
	 * DEFAULT expressions need to be evaluated in a per-tuple context, so
	 * switch in case we are doing that.
	 */
	MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

	/*
	 * The protocol for loading a virtual tuple into a slot is first
	 * ExecClearTuple, then fill the values/isnull arrays, then
	 * ExecStoreVirtualTuple.  If we don't find another row in the file, we
	 * just skip the last step, leaving the slot empty as required.
	 *
	 */
	ExecClearTuple(slot);

	if (NextCopyFrom(cstate, econtext, slot->tts_values, slot->tts_isnull))
	{
		if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
			cstate->escontext->error_occurred)
		{
			/*
			 * Soft error occurred, skip this tuple and just make
			 * ErrorSaveContext ready for the next NextCopyFrom. Since we
			 * don't set details_wanted and error_data is not to be filled,
			 * just resetting error_occurred is enough.
			 */
			cstate->escontext->error_occurred = false;

			/* Switch back to original memory context */
			MemoryContextSwitchTo(oldcontext);

			/*
			 * Make sure we are interruptible while repeatedly calling
			 * NextCopyFrom() until no soft error occurs.
			 */
			CHECK_FOR_INTERRUPTS();

			/*
			 * Reset the per-tuple exprcontext, to clean-up after expression
			 * evaluations etc.
			 */
			ResetPerTupleExprContext(estate);

			if (cstate->opts.reject_limit > 0 &&
				cstate->num_errors > cstate->opts.reject_limit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("skipped more than REJECT_LIMIT (%lld) rows due to data type incompatibility",
								(long long) cstate->opts.reject_limit)));

			/* Repeat NextCopyFrom() until no soft error occurs */
			goto retry;
		}

		ExecStoreVirtualTuple(slot);
	}

	/* Switch back to original memory context */
	MemoryContextSwitchTo(oldcontext);

	/* Remove error callback. */
	error_context_stack = errcallback.previous;

	return slot;
}

/*
 * fileReScanForeignScan
 *		Rescan table, possibly with new parameters
 */
static void
fileReScanForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;

	EndCopyFrom(festate->cstate);

	festate->cstate = BeginCopyFrom(NULL,
									node->ss.ss_currentRelation,
									NULL,
									festate->filename,
									festate->is_program,
									NULL,
									NIL,
									festate->options);
}

/*
 * fileEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
fileEndForeignScan(ForeignScanState *node)
{
	FileFdwExecutionState *festate = (FileFdwExecutionState *) node->fdw_state;

	/* if festate is NULL, we are in EXPLAIN; nothing to do */
	if (!festate)
		return;

	if (festate->cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
		festate->cstate->num_errors > 0 &&
		festate->cstate->opts.log_verbosity >= COPY_LOG_VERBOSITY_DEFAULT)
		ereport(NOTICE,
				errmsg_plural("%llu row was skipped due to data type incompatibility",
							  "%llu rows were skipped due to data type incompatibility",
							  (unsigned long long) festate->cstate->num_errors,
							  (unsigned long long) festate->cstate->num_errors));

	EndCopyFrom(festate->cstate);
}

/*
 * fileAnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 */
static bool
fileAnalyzeForeignTable(Relation relation,
						AcquireSampleRowsFunc *func,
						BlockNumber *totalpages)
{
	char	   *filename;
	bool		is_program;
	List	   *options;
	struct stat stat_buf;

	/* Fetch options of foreign table */
	fileGetOptions(RelationGetRelid(relation), &filename, &is_program, &options);

	/*
	 * If this is a program instead of a file, just return false to skip
	 * analyzing the table.  We could run the program and collect stats on
	 * whatever it currently returns, but it seems likely that in such cases
	 * the output would be too volatile for the stats to be useful.  Maybe
	 * there should be an option to enable doing this?
	 */
	if (is_program)
		return false;

	/*
	 * Get size of the file.  (XXX if we fail here, would it be better to just
	 * return false to skip analyzing the table?)
	 */
	if (stat(filename, &stat_buf) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						filename)));

	/*
	 * Convert size to pages.  Must return at least 1 so that we can tell
	 * later on that pg_class.relpages is not default.
	 */
	*totalpages = (stat_buf.st_size + (BLCKSZ - 1)) / BLCKSZ;
	if (*totalpages < 1)
		*totalpages = 1;

	*func = file_acquire_sample_rows;

	return true;
}

/*
 * fileIsForeignScanParallelSafe
 *		Reading a file, or external program, in a parallel worker should work
 *		just the same as reading it in the leader, so mark scans safe.
 */
static bool
fileIsForeignScanParallelSafe(PlannerInfo *root, RelOptInfo *rel,
							  RangeTblEntry *rte)
{
	return true;
}

/*
 * check_selective_binary_conversion
 *
 * Check to see if it's useful to convert only a subset of the file's columns
 * to binary.  If so, construct a list of the column names to be converted,
 * return that at *columns, and return true.  (Note that it's possible to
 * determine that no columns need be converted, for instance with a COUNT(*)
 * query.  So we can't use returning a NIL list to indicate failure.)
 */
static bool
check_selective_binary_conversion(RelOptInfo *baserel,
								  Oid foreigntableid,
								  List **columns)
{
	ForeignTable *table;
	ListCell   *lc;
	Relation	rel;
	TupleDesc	tupleDesc;
	int			attidx;
	Bitmapset  *attrs_used = NULL;
	bool		has_wholerow = false;
	int			numattrs;
	int			i;

	*columns = NIL;				/* default result */

	/*
	 * Check format of the file.  If binary format, this is irrelevant.
	 */
	table = GetForeignTable(foreigntableid);
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "format") == 0)
		{
			char	   *format = defGetString(def);

			if (strcmp(format, "binary") == 0)
				return false;
			break;
		}
	}

	/* Collect all the attributes needed for joins or final output. */
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
				   &attrs_used);

	/* Add all the attributes used by restriction clauses. */
	foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &attrs_used);
	}

	/* Convert attribute numbers to column names. */
	rel = table_open(foreigntableid, AccessShareLock);
	tupleDesc = RelationGetDescr(rel);

	attidx = -1;
	while ((attidx = bms_next_member(attrs_used, attidx)) >= 0)
	{
		/* attidx is zero-based, attnum is the normal attribute number */
		AttrNumber	attnum = attidx + FirstLowInvalidHeapAttributeNumber;

		if (attnum == 0)
		{
			has_wholerow = true;
			break;
		}

		/* Ignore system attributes. */
		if (attnum < 0)
			continue;

		/* Get user attributes. */
		if (attnum > 0)
		{
			Form_pg_attribute attr = TupleDescAttr(tupleDesc, attnum - 1);
			char	   *attname = NameStr(attr->attname);

			/* Skip dropped attributes (probably shouldn't see any here). */
			if (attr->attisdropped)
				continue;

			/*
			 * Skip generated columns (COPY won't accept them in the column
			 * list)
			 */
			if (attr->attgenerated)
				continue;
			*columns = lappend(*columns, makeString(pstrdup(attname)));
		}
	}

	/* Count non-dropped user attributes while we have the tupdesc. */
	numattrs = 0;
	for (i = 0; i < tupleDesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupleDesc, i);

		if (attr->attisdropped)
			continue;
		numattrs++;
	}

	table_close(rel, AccessShareLock);

	/* If there's a whole-row reference, fail: we need all the columns. */
	if (has_wholerow)
	{
		*columns = NIL;
		return false;
	}

	/* If all the user attributes are needed, fail. */
	if (numattrs == list_length(*columns))
	{
		*columns = NIL;
		return false;
	}

	return true;
}

/*
 * Estimate size of a foreign table.
 *
 * The main result is returned in baserel->rows.  We also set
 * fdw_private->pages and fdw_private->ntuples for later use in the cost
 * calculation.
 */
static void
estimate_size(PlannerInfo *root, RelOptInfo *baserel,
			  FileFdwPlanState *fdw_private)
{
	struct stat stat_buf;
	BlockNumber pages;
	double		ntuples;
	double		nrows;

	/*
	 * Get size of the file.  It might not be there at plan time, though, in
	 * which case we have to use a default estimate.  We also have to fall
	 * back to the default if using a program as the input.
	 */
	if (fdw_private->is_program || stat(fdw_private->filename, &stat_buf) < 0)
		stat_buf.st_size = 10 * BLCKSZ;

	/*
	 * Convert size to pages for use in I/O cost estimate later.
	 */
	pages = (stat_buf.st_size + (BLCKSZ - 1)) / BLCKSZ;
	if (pages < 1)
		pages = 1;
	fdw_private->pages = pages;

	/*
	 * Estimate the number of tuples in the file.
	 */
	if (baserel->tuples >= 0 && baserel->pages > 0)
	{
		/*
		 * We have # of pages and # of tuples from pg_class (that is, from a
		 * previous ANALYZE), so compute a tuples-per-page estimate and scale
		 * that by the current file size.
		 */
		double		density;

		density = baserel->tuples / (double) baserel->pages;
		ntuples = clamp_row_est(density * (double) pages);
	}
	else
	{
		/*
		 * Otherwise we have to fake it.  We back into this estimate using the
		 * planner's idea of the relation width; which is bogus if not all
		 * columns are being read, not to mention that the text representation
		 * of a row probably isn't the same size as its internal
		 * representation.  Possibly we could do something better, but the
		 * real answer to anyone who complains is "ANALYZE" ...
		 */
		int			tuple_width;

		tuple_width = MAXALIGN(baserel->reltarget->width) +
			MAXALIGN(SizeofHeapTupleHeader);
		ntuples = clamp_row_est((double) stat_buf.st_size /
								(double) tuple_width);
	}
	fdw_private->ntuples = ntuples;

	/*
	 * Now estimate the number of rows returned by the scan after applying the
	 * baserestrictinfo quals.
	 */
	nrows = ntuples *
		clauselist_selectivity(root,
							   baserel->baserestrictinfo,
							   0,
							   JOIN_INNER,
							   NULL);

	nrows = clamp_row_est(nrows);

	/* Save the output-rows estimate for the planner */
	baserel->rows = nrows;
}

/*
 * Estimate costs of scanning a foreign table.
 *
 * Results are returned in *startup_cost and *total_cost.
 */
static void
estimate_costs(PlannerInfo *root, RelOptInfo *baserel,
			   FileFdwPlanState *fdw_private,
			   Cost *startup_cost, Cost *total_cost)
{
	BlockNumber pages = fdw_private->pages;
	double		ntuples = fdw_private->ntuples;
	Cost		run_cost = 0;
	Cost		cpu_per_tuple;

	/*
	 * We estimate costs almost the same way as cost_seqscan(), thus assuming
	 * that I/O costs are equivalent to a regular table file of the same size.
	 * However, we take per-tuple CPU costs as 10x of a seqscan, to account
	 * for the cost of parsing records.
	 *
	 * In the case of a program source, this calculation is even more divorced
	 * from reality, but we have no good alternative; and it's not clear that
	 * the numbers we produce here matter much anyway, since there's only one
	 * access path for the rel.
	 */
	run_cost += seq_page_cost * pages;

	*startup_cost = baserel->baserestrictcost.startup;
	cpu_per_tuple = cpu_tuple_cost * 10 + baserel->baserestrictcost.per_tuple;
	run_cost += cpu_per_tuple * ntuples;
	*total_cost = *startup_cost + run_cost;
}

/*
 * file_acquire_sample_rows -- acquire a random sample of rows from the table
 *
 * Selected rows are returned in the caller-allocated array rows[],
 * which must have at least targrows entries.
 * The actual number of rows selected is returned as the function result.
 * We also count the total number of rows in the file and return it into
 * *totalrows.  Rows skipped due to on_error = 'ignore' are not included
 * in this count.  Note that *totaldeadrows is always set to 0.
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the file.  Therefore, correlation estimates derived later
 * may be meaningless, but it's OK because we don't use the estimates
 * currently (the planner only pays attention to correlation for indexscans).
 */
static int
file_acquire_sample_rows(Relation onerel, int elevel,
						 HeapTuple *rows, int targrows,
						 double *totalrows, double *totaldeadrows)
{
	int			numrows = 0;
	double		rowstoskip = -1;	/* -1 means not set yet */
	ReservoirStateData rstate;
	TupleDesc	tupDesc;
	Datum	   *values;
	bool	   *nulls;
	bool		found;
	char	   *filename;
	bool		is_program;
	List	   *options;
	CopyFromState cstate;
	ErrorContextCallback errcallback;
	MemoryContext oldcontext = CurrentMemoryContext;
	MemoryContext tupcontext;

	Assert(onerel);
	Assert(targrows > 0);

	tupDesc = RelationGetDescr(onerel);
	values = (Datum *) palloc(tupDesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupDesc->natts * sizeof(bool));

	/* Fetch options of foreign table */
	fileGetOptions(RelationGetRelid(onerel), &filename, &is_program, &options);

	/*
	 * Create CopyState from FDW options.
	 */
	cstate = BeginCopyFrom(NULL, onerel, NULL, filename, is_program, NULL, NIL,
						   options);

	/*
	 * Use per-tuple memory context to prevent leak of memory used to read
	 * rows from the file with Copy routines.
	 */
	tupcontext = AllocSetContextCreate(CurrentMemoryContext,
									   "file_fdw temporary context",
									   ALLOCSET_DEFAULT_SIZES);

	/* Prepare for sampling rows */
	reservoir_init_selection_state(&rstate, targrows);

	/* Set up callback to identify error line number. */
	errcallback.callback = CopyFromErrorCallback;
	errcallback.arg = cstate;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	*totalrows = 0;
	*totaldeadrows = 0;
	for (;;)
	{
		/* Check for user-requested abort or sleep */
		vacuum_delay_point();

		/* Fetch next row */
		MemoryContextReset(tupcontext);
		MemoryContextSwitchTo(tupcontext);

		found = NextCopyFrom(cstate, NULL, values, nulls);

		MemoryContextSwitchTo(oldcontext);

		if (!found)
			break;

		if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
			cstate->escontext->error_occurred)
		{
			/*
			 * Soft error occurred, skip this tuple and just make
			 * ErrorSaveContext ready for the next NextCopyFrom. Since we
			 * don't set details_wanted and error_data is not to be filled,
			 * just resetting error_occurred is enough.
			 */
			cstate->escontext->error_occurred = false;

			/* Repeat NextCopyFrom() until no soft error occurs */
			continue;
		}

		/*
		 * The first targrows sample rows are simply copied into the
		 * reservoir.  Then we start replacing tuples in the sample until we
		 * reach the end of the relation. This algorithm is from Jeff Vitter's
		 * paper (see more info in commands/analyze.c).
		 */
		if (numrows < targrows)
		{
			rows[numrows++] = heap_form_tuple(tupDesc, values, nulls);
		}
		else
		{
			/*
			 * t in Vitter's paper is the number of records already processed.
			 * If we need to compute a new S value, we must use the
			 * not-yet-incremented value of totalrows as t.
			 */
			if (rowstoskip < 0)
				rowstoskip = reservoir_get_next_S(&rstate, *totalrows, targrows);

			if (rowstoskip <= 0)
			{
				/*
				 * Found a suitable tuple, so save it, replacing one old tuple
				 * at random
				 */
				int			k = (int) (targrows * sampler_random_fract(&rstate.randstate));

				Assert(k >= 0 && k < targrows);
				heap_freetuple(rows[k]);
				rows[k] = heap_form_tuple(tupDesc, values, nulls);
			}

			rowstoskip -= 1;
		}

		*totalrows += 1;
	}

	/* Remove error callback. */
	error_context_stack = errcallback.previous;

	/* Clean up. */
	MemoryContextDelete(tupcontext);

	if (cstate->opts.on_error == COPY_ON_ERROR_IGNORE &&
		cstate->num_errors > 0 &&
		cstate->opts.log_verbosity >= COPY_LOG_VERBOSITY_DEFAULT)
		ereport(NOTICE,
				errmsg_plural("%llu row was skipped due to data type incompatibility",
							  "%llu rows were skipped due to data type incompatibility",
							  (unsigned long long) cstate->num_errors,
							  (unsigned long long) cstate->num_errors));

	EndCopyFrom(cstate);

	pfree(values);
	pfree(nulls);

	/*
	 * Emit some interesting relation info
	 */
	ereport(elevel,
			(errmsg("\"%s\": file contains %.0f rows; "
					"%d rows in sample",
					RelationGetRelationName(onerel),
					*totalrows, numrows)));

	return numrows;
}
